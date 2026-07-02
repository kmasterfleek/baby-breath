const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('babybreath', {
  getServerStatus: () => ipcRenderer.invoke('get-server-status'),
  getLocalIP: () => ipcRenderer.invoke('get-local-ip'),
  listSerialPorts: () => ipcRenderer.invoke('list-serial-ports'),
  provisionBoard: (opts) => ipcRenderer.invoke('provision-board', opts),
  navigate: (page) => ipcRenderer.invoke('navigate', page),
  platform: process.platform,
});
