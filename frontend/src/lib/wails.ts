import type {
  HealthReport,
  HotkeyTestResult,
  LockAllResult,
  RepairResult,
  Settings,
  StatusResponse,
  SystemInfo,
} from '../types';

declare global {
  interface Window {
    go?: {
      main?: {
        App?: {
          GetSystemInfo(): Promise<SystemInfo>;
          GetStatus(): Promise<StatusResponse>;
          SelectDirectory(defaultDir: string): Promise<string>;
          FilterDirectories(paths: string[]): Promise<string[]>;
          SetMasterPassword(oldPassword: string, newPassword: string): Promise<StatusResponse>;
          VerifyMaster(password: string): Promise<boolean>;
          UpdateSettings(password: string, settings: Settings): Promise<StatusResponse>;
          TestHotkey(hotkey: string): Promise<HotkeyTestResult>;
          AddAndLock(path: string, useCustom: boolean, customPassword: string): Promise<StatusResponse>;
          LockDir(id: string, path: string): Promise<StatusResponse>;
          LockAll(): Promise<LockAllResult>;
          UnlockDir(id: string, path: string, password: string, openAfter: boolean): Promise<StatusResponse>;
          ChangeDirPassword(
            id: string,
            currentPassword: string,
            useCustom: boolean,
            newCustomPassword: string
          ): Promise<StatusResponse>;
          RemoveDir(id: string, password: string): Promise<StatusResponse>;
          Diagnose(password: string): Promise<HealthReport>;
          Repair(password: string, cleanDeadRecords: boolean): Promise<RepairResult>;
          OpenFolder(path: string): Promise<void>;
          OpenURL(url: string): Promise<void>;
          SyncWindowCorners(): Promise<void>;
          HideToTrayOrQuit(): Promise<void>;
          CompletePostInstall(launchMain: boolean): Promise<void>;
          ForceQuit(relockAll: boolean): Promise<void>;
        };
      };
    };
    runtime?: {
      EventsOn(eventName: string, callback: (...data: any[]) => void): () => void;
      EventsOff(eventName: string, ...additionalEventNames: string[]): void;
      OnFileDrop?(
        callback: (x: number, y: number, paths: string[]) => void,
        useDropTarget?: boolean
      ): void;
      OnFileDropOff?(): void;
      WindowMinimise?(): void;
      WindowToggleMaximise?(): void;
      WindowIsMaximised?(): Promise<boolean>;
      Quit?(): void;
    };
  }
}

export const backend = {
  async getSystemInfo(): Promise<SystemInfo> {
    if (window.go?.main?.App) return window.go.main.App.GetSystemInfo();
    return {
      os: 'windows',
      arch: 'amd64',
      appVersion: '3.0.0',
      statePath: '%LOCALAPPDATA%\\DirLocker\\state.json',
      homeDir: 'D:\\工作文档',
      trayAvailable: true,
      startMode: 'normal',
    };
  },

  async getStatus(): Promise<StatusResponse> {
    if (window.go?.main?.App) return window.go.main.App.GetStatus();
    return {
      has_master_password: true,
      daemon_connected: true,
      hotkey_active: true,
      tray_available: true,
      settings: {
        auto_relock_minutes: 5,
        require_master_on_open: false,
        allow_in_place_fallback: false,
        hotkey: 'Ctrl+Alt+K',
      },
      entries: [],
    };
  },

  async selectDirectory(defaultDir = ''): Promise<string> {
    if (window.go?.main?.App) return window.go.main.App.SelectDirectory(defaultDir);
    return '';
  },

  async filterDirectories(paths: string[]): Promise<string[]> {
    if (window.go?.main?.App) return window.go.main.App.FilterDirectories(paths);
    return paths;
  },

  async setMasterPassword(oldPassword: string, newPassword: string): Promise<StatusResponse> {
    return window.go!.main!.App!.SetMasterPassword(oldPassword, newPassword);
  },

  async verifyMaster(password: string): Promise<boolean> {
    return window.go!.main!.App!.VerifyMaster(password);
  },

  async updateSettings(password: string, settings: Settings): Promise<StatusResponse> {
    return window.go!.main!.App!.UpdateSettings(password, settings);
  },

  async testHotkey(hotkey: string): Promise<HotkeyTestResult> {
    if (window.go?.main?.App) return window.go.main.App.TestHotkey(hotkey);
    return { ok: true, message: `快捷键 ${hotkey} 可用` };
  },

  async addAndLock(path: string, useCustom: boolean, customPassword: string): Promise<StatusResponse> {
    return window.go!.main!.App!.AddAndLock(path, useCustom, customPassword);
  },

  async lockDir(id: string, path: string): Promise<StatusResponse> {
    return window.go!.main!.App!.LockDir(id, path);
  },

  async lockAll(): Promise<LockAllResult> {
    return window.go!.main!.App!.LockAll();
  },

  async unlockDir(
    id: string,
    path: string,
    password: string,
    openAfter: boolean
  ): Promise<StatusResponse> {
    return window.go!.main!.App!.UnlockDir(id, path, password, openAfter);
  },

  async changeDirPassword(
    id: string,
    currentPassword: string,
    useCustom: boolean,
    newCustomPassword: string
  ): Promise<StatusResponse> {
    return window.go!.main!.App!.ChangeDirPassword(
      id,
      currentPassword,
      useCustom,
      newCustomPassword
    );
  },

  async removeDir(id: string, password: string): Promise<StatusResponse> {
    return window.go!.main!.App!.RemoveDir(id, password);
  },

  async diagnose(password: string): Promise<HealthReport> {
    return window.go!.main!.App!.Diagnose(password);
  },

  async repair(password: string, cleanDeadRecords: boolean): Promise<RepairResult> {
    return window.go!.main!.App!.Repair(password, cleanDeadRecords);
  },

  async openFolder(path: string): Promise<void> {
    return window.go?.main?.App?.OpenFolder(path);
  },

  async openURL(url: string): Promise<void> {
    if (window.go?.main?.App) return window.go.main.App.OpenURL(url);
    window.open(url, '_blank', 'noopener,noreferrer');
  },

  syncWindowCorners(): void {
    window.go?.main?.App?.SyncWindowCorners?.().catch(() => {});
  },

  hideToTrayOrQuit(): void {
    window.go?.main?.App?.HideToTrayOrQuit?.().catch(() => {});
  },

  completePostInstall(launchMain: boolean): void {
    window.go?.main?.App?.CompletePostInstall?.(launchMain).catch(() => {});
  },

  forceQuit(relockAll: boolean): void {
    window.go?.main?.App?.ForceQuit?.(relockAll).catch(() => {});
  },

  onEvent<T>(eventName: string, cb: (payload: T) => void): () => void {
    if (window.runtime?.EventsOn) {
      return window.runtime.EventsOn(eventName, (data: T) => cb(data));
    }
    return () => {};
  },

  onFileDrop(cb: (paths: string[]) => void, useDropTarget = true): () => void {
    if (window.runtime?.OnFileDrop) {
      window.runtime.OnFileDrop((_x, _y, paths) => {
        cb(Array.isArray(paths) ? paths : []);
      }, useDropTarget);
      return () => {
        window.runtime?.OnFileDropOff?.();
      };
    }
    return () => {};
  },

  windowMinimise(): void {
    window.runtime?.WindowMinimise?.();
  },

  windowToggleMaximise(): void {
    window.runtime?.WindowToggleMaximise?.();
  },

  async windowIsMaximised(): Promise<boolean> {
    if (window.runtime?.WindowIsMaximised) {
      return window.runtime.WindowIsMaximised();
    }
    return false;
  },
};
