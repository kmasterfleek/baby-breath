const { app, BrowserWindow, ipcMain, dialog, powerSaveBlocker } = require('electron');
const path = require('path');
const { spawn, execFile } = require('child_process');
const fs = require('fs');

let mainWindow;
let serverProcess = null;
let quitting = false;          // suppress auto-restart during shutdown
let restartAttempts = 0;
let restartTimer = null;
let powerBlockerId = null;
const SERVER_HTTP_PORT = 8080;
const SERVER_WS_PORT = 8765;
const SERVER_UDP_PORT = 5005;

// ─── Paths ──────────────────────────────────────────────────
function getServerBinary() {
  // In dev: use the built binary from the workspace
  const devPath = path.join(__dirname, '..', 'rust-port', 'wifi-densepose-rs', 'target', 'release', 'sensing-server');
  if (fs.existsSync(devPath)) return devPath;

  // In packaged app: look in resources/bin/
  const prodPath = path.join(process.resourcesPath, 'bin', 'sensing-server');
  if (fs.existsSync(prodPath)) return prodPath;

  return null;
}

function getUIPath() {
  // In packaged app the UI must live on real disk (extraResources) — the Rust
  // server is a separate process and cannot read files inside app.asar.
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'ui');
  }
  const devPath = path.join(__dirname, 'ui');
  if (fs.existsSync(devPath)) return devPath;
  return path.join(__dirname, '..', 'ui');
}

// ─── Server Management ──────────────────────────────────────
// A bind-probe (net.listen) is unreliable on macOS: SO_REUSEADDR lets a
// 127.0.0.1 bind succeed while another process holds *:port. Probe /health
// over HTTP instead — an answer is definitive proof a server is up.
async function isServerResponding() {
  try {
    const resp = await fetch(`http://127.0.0.1:${SERVER_HTTP_PORT}/health`, {
      signal: AbortSignal.timeout(1000),
    });
    return resp.ok;
  } catch (e) {
    return false;
  }
}

async function startServer() {
  const binary = getServerBinary();
  if (!binary) {
    console.error('Sensing server binary not found');
    return false;
  }

  // Check if already running
  if (await isServerResponding()) {
    console.log('Server already running on port', SERVER_HTTP_PORT);
    if (powerBlockerId === null) {
      powerBlockerId = powerSaveBlocker.start('prevent-app-suspension');
    }
    return true;
  }

  const uiPath = getUIPath();
  console.log('Starting server:', binary);
  console.log('UI path:', uiPath);

  serverProcess = spawn(binary, [
    '--bind-addr', '0.0.0.0',
    '--source', 'esp32',
    '--ui-path', uiPath,
    '--http-port', String(SERVER_HTTP_PORT),
    '--ws-port', String(SERVER_WS_PORT),
    '--udp-port', String(SERVER_UDP_PORT),
  ], {
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env },
  });

  serverProcess.stdout.on('data', (data) => {
    console.log('[server]', data.toString().trim());
  });

  serverProcess.stderr.on('data', (data) => {
    console.error('[server]', data.toString().trim());
  });

  serverProcess.on('exit', (code) => {
    console.log('Server exited with code', code);
    serverProcess = null;
    // Overnight monitoring must survive a server crash: respawn with backoff
    // unless the app is quitting (an overnight recording died this way once).
    if (!quitting) {
      const delay = Math.min(30000, 2000 * Math.pow(2, restartAttempts));
      restartAttempts++;
      console.log(`Server crashed — restarting in ${delay / 1000}s (attempt ${restartAttempts})`);
      restartTimer = setTimeout(async () => {
        restartTimer = null;
        const ok = await startServer();
        if (ok) restartAttempts = 0;
      }, delay);
    }
  });

  // Wait for server to be ready. Startup scans the recordings dir, which can
  // take a while on slow disks, so allow a generous window (the loop bails
  // early if the process dies).
  for (let i = 0; i < 120; i++) {
    await new Promise(r => setTimeout(r, 500));
    if (serverProcess === null) {
      console.error('Server process exited during startup');
      return false;
    }
    if (await isServerResponding()) {
      console.log('Server ready');
      // Monitoring runs overnight — keep the system from sleeping while the
      // server is up (display may still sleep).
      if (powerBlockerId === null) {
        powerBlockerId = powerSaveBlocker.start('prevent-app-suspension');
        console.log('Keep-awake enabled (powerSaveBlocker', powerBlockerId + ')');
      }
      return true;
    }
  }

  console.error('Server failed to start within 60 seconds');
  return false;
}

function stopServer() {
  if (restartTimer) {
    clearTimeout(restartTimer);
    restartTimer = null;
  }
  if (powerBlockerId !== null) {
    powerSaveBlocker.stop(powerBlockerId);
    powerBlockerId = null;
  }
  if (serverProcess) {
    console.log('Stopping server...');
    serverProcess.kill('SIGTERM');
    serverProcess = null;
  }
}

// ─── Window ─────────────────────────────────────────────────
async function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: 900,
    minHeight: 600,
    title: 'My Baby',
    titleBarStyle: 'hiddenInset',
    backgroundColor: '#0b0b12',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
      // Enable Web Serial API for USB provisioning
      enableWebSerial: true,
    },
  });

  // Start the sensing server
  const serverOk = await startServer();

  if (serverOk) {
    // Check if any nodes are reporting — if not, show setup wizard (first run)
    let hasNodes = false;
    try {
      const resp = await fetch(`http://localhost:${SERVER_HTTP_PORT}/api/v1/nodes`);
      const data = await resp.json();
      hasNodes = data.nodes && data.nodes.length > 0;
    } catch(e) {}

    // Also check if USB sensors are plugged in (setup scenario)
    const ports = findSerialPorts();
    const freshSetup = !hasNodes && ports.length > 0;

    if (freshSetup) {
      // Customer just plugged in new sensors — show setup
      mainWindow.loadFile(path.join(getUIPath(), 'setup.html'));
    } else {
      // Normal operation — show monitor
      mainWindow.loadURL(`http://localhost:${SERVER_HTTP_PORT}/ui/baby.html`);
    }
  } else {
    mainWindow.loadFile(path.join(getUIPath(), 'setup.html'));
  }

  // Handle Web Serial permission requests
  mainWindow.webContents.session.on('select-serial-port', (event, portList, webContents, callback) => {
    event.preventDefault();
    // Auto-select ESP32 devices, or show all if none match
    const esp = portList.find(p =>
      p.displayName?.includes('ESP') ||
      p.usbVendorId === 0x303A || // Espressif VID
      p.usbProductId === 0x1001
    );
    callback(esp ? esp.portId : (portList[0]?.portId || ''));
  });

  mainWindow.webContents.session.on('serial-port-added', (event, port) => {
    console.log('Serial port added:', port.portName);
  });

  mainWindow.on('closed', () => { mainWindow = null; });
}

// ─── App Lifecycle ──────────────────────────────────────────
app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  // On macOS, closing the window must NOT stop monitoring — the server (and
  // any recording) keeps running in the background; the dock icon reopens the
  // window. Quit explicitly with Cmd+Q to stop everything.
  if (process.platform !== 'darwin') {
    quitting = true;
    stopServer();
    app.quit();
  }
});

app.on('before-quit', () => {
  quitting = true;
  stopServer();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});

// ─── IPC: Provisioning ─────────────────────────────────────

function getLocalIP() {
  const { networkInterfaces } = require('os');
  const nets = networkInterfaces();
  for (const name of Object.keys(nets)) {
    for (const iface of nets[name]) {
      if (iface.family === 'IPv4' && !iface.internal && iface.address.startsWith('192.168')) {
        return iface.address;
      }
    }
  }
  // Fallback: any non-internal IPv4
  for (const name of Object.keys(nets)) {
    for (const iface of nets[name]) {
      if (iface.family === 'IPv4' && !iface.internal) return iface.address;
    }
  }
  return '127.0.0.1';
}

function findSerialPorts() {
  // Look for ESP32 USB serial ports
  const glob = require('path');
  try {
    const files = fs.readdirSync('/dev').filter(f => f.startsWith('cu.usbmodem'));
    return files.map(f => '/dev/' + f);
  } catch(e) { return []; }
}

ipcMain.handle('get-server-status', async () => {
  const running = await isServerResponding();
  return { running, port: SERVER_HTTP_PORT };
});

ipcMain.handle('get-local-ip', async () => {
  return getLocalIP();
});

ipcMain.handle('list-serial-ports', async () => {
  return findSerialPorts();
});

function getProvisionScript() {
  const devPath = path.join(__dirname, '..', 'firmware', 'esp32-csi-node', 'provision.py');
  if (fs.existsSync(devPath)) return devPath;
  return path.join(process.resourcesPath, 'firmware', 'provision.py');
}

ipcMain.handle('provision-board', async (event, { port, ssid, password, nodeId }) => {
  const provisionScript = getProvisionScript();
  const localIP = getLocalIP();

  // Use ESP-IDF's Python env if available, else system python3
  const pythonPaths = [
    path.join(process.env.HOME || '', '.espressif', 'python_env', 'idf5.4_py3.14_env', 'bin', 'python3'),
    'python3'
  ];
  let python = pythonPaths.find(p => { try { fs.accessSync(p.startsWith('/') ? p : p); return true; } catch(e) { return false; } }) || 'python3';

  // Prefer ESP-IDF python for nvs partition gen
  const espPython = path.join(process.env.HOME || '', '.espressif', 'python_env', 'idf5.4_py3.14_env', 'bin', 'python3');
  if (fs.existsSync(espPython)) python = espPython;

  return new Promise((resolve, reject) => {
    console.log(`Provisioning ${port} as node ${nodeId} → ${localIP}:${SERVER_UDP_PORT}`);
    execFile(python, [
      provisionScript,
      '--port', port,
      '--ssid', ssid,
      '--password', password,
      '--target-ip', localIP,
      '--target-port', String(SERVER_UDP_PORT),
      '--node-id', String(nodeId),
      '--tdm-slot', String(nodeId - 1),
      '--tdm-total', '2',
    ], { timeout: 30000 }, (error, stdout, stderr) => {
      if (error) {
        console.error('Provision error:', error.message, stderr);
        reject(stderr || error.message);
      } else {
        console.log('Provision OK:', stdout);
        resolve({ success: true, ip: localIP, output: stdout });
      }
    });
  });
});

ipcMain.handle('navigate', async (event, page) => {
  if (page === 'monitor') {
    mainWindow.loadURL(`http://localhost:${SERVER_HTTP_PORT}/ui/baby.html`);
  } else if (page === 'setup') {
    mainWindow.loadFile(path.join(getUIPath(), 'setup.html'));
  }
});
