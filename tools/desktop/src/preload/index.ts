import { contextBridge, ipcRenderer } from "electron";
import type { DesktopApi, DesktopEvent } from "../shared/contracts";

const invoke = <T>(method: string, input?: unknown): Promise<T> =>
  ipcRenderer.invoke(`desktop:${method}`, input);
const api: DesktopApi = {
  workspace: () => invoke("workspace"),
  saveConnection: (input) => invoke("saveConnection", input),
  deleteConnection: (id) => invoke("deleteConnection", id),
  preferences: (input) => invoke("preferences", input),
  connect: (id) => invoke("connect", id),
  disconnect: (id) => invoke("disconnect", id),
  execute: (input) => invoke("execute", input),
  cancel: (id) => invoke("cancel", id),
  monitoring: (id) => invoke("monitoring", id),
  topology: (id) => invoke("topology", id),
  metrics: (id) => invoke("metrics", id),
  pickBinary: () => invoke("pickBinary"),
  startLocal: (input) => invoke("startLocal", input),
  stopLocal: () => invoke("stopLocal"),
  connectLocal: (id) => invoke("connectLocal", id),
  copyText: (text) => invoke("copyText", text),
  stopNode: (id) => invoke("stopNode", id),
  localState: () => invoke("localState"),
  runTask: (input) => invoke("runTask", input),
  stopTask: () => invoke("stopTask"),
  taskState: () => invoke("taskState"),
  logs: () => invoke("logs"),
  exportText: (input) => invoke("exportText", input),
  onEvent(callback) {
    const listener = (
      _event: Electron.IpcRendererEvent,
      event: DesktopEvent,
    ): void => callback(event);
    ipcRenderer.on("desktop:event", listener);
    return () => ipcRenderer.removeListener("desktop:event", listener);
  },
};
contextBridge.exposeInMainWorld("desktop", Object.freeze(api));
