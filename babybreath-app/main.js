const { app, BrowserWindow, ipcMain, dialog } = require('electron');
const path = require('path');
const { spawn, execFile } = require('child_process');
const fs = require('fs');
const net = require('net');

let mainWindow;
let serverProcess = null;
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
  const devPath = path.join(__dirname, 'ui');
  if (fs.existsSync(devPath)) return devPath;
  return path.join(__dirname, '..', 'ui');
}

// ─── Server Management ──────────────────────────────────────
function isPortFree(port) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', () => resolve(false));
    server.once('listening', () => { server.close(); resolve(true); });
    server.listen(port, '127.0.0.1');
  });
}

async function startServer() {
  const binary = getServerBinary();
  if (!binary) {
    console.error('Sensing server binary not found');
    return false;
  }

  // Check if already running
  const free = await isPortFree(SERVER_HTTP_PORT);
  if (!free) {
    console.log('Server already running on port', SERVER_HTTP_PORT);
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
  });

  // Wait for server to be ready
  for (let i = 0; i < 30; i++) {
    await new Promise(r => setTimeout(r, 200));
    const ready = !(await isPortFree(SERVER_HTTP_PORT));
    if (ready) {
      console.log('Server ready');
      return true;
    }
  }

  console.error('Server failed to start within 6 seconds');
  return false;
}

function stopServer() {
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
  stopServer();
  app.quit();
});

app.on('before-quit', () => {
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
  const free = await isPortFree(SERVER_HTTP_PORT);
  return { running: !free, port: SERVER_HTTP_PORT };
});

ipcMain.handle('get-local-ip', async () => {
  return getLocalIP();
});

ipcMain.handle('list-serial-ports', async () => {
  return findSerialPorts();
});

ipcMain.handle('provision-board', async (event, { port, ssid, password, nodeId }) => {
  const provisionScript = path.join(__dirname, '..', 'firmware', 'esp32-csi-node', 'provision.py');
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
