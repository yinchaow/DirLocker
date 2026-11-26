import React, { useCallback, useEffect, useMemo, useState } from 'react';
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  Copy,
  FolderOpen,
  FolderPlus,
  Info,
  KeyRound,
  LifeBuoy,
  Lock,
  Maximize2,
  Minimize2,
  Minus,
  Moon,
  Power,
  RefreshCw,
  Search,
  Settings as SettingsIcon,
  Shield,
  ShieldCheck,
  Sun,
  Trash2,
  Unlock,
  X,
} from 'lucide-react';
import type {
  ActionRequestEvent,
  DirEntry,
  NotifyTone,
  Settings,
  StatusResponse,
  SystemInfo,
} from './types';
import { backend } from './lib/wails';
import { AppLogo, LockedFolderIcon, PasswordInput, UnlockedFolderIcon } from './components/FluentIcons';
import { AboutModal } from './components/AboutModal';
import {
  ChangeDirPasswordModal,
  LockDirModal,
  MasterPasswordModal,
  RemoveDirModal,
  UnlockDirModal,
} from './components/PasswordModals';
import {
  HealthCheckModal,
  QuitConfirmModal,
  RecoveryModal,
  SettingsModal,
} from './components/SystemModals';

type FilterTab = 'all' | 'locked' | 'unlocked' | 'custom';

interface ToastState {
  id: number;
  message: string;
  tone: NotifyTone;
}

const DEFAULT_SETTINGS: Settings = {
  auto_relock_minutes: 5,
  require_master_on_open: false,
  allow_in_place_fallback: false,
  hotkey: 'Ctrl+Alt+K',
};

function leafName(p: string): string {
  return p.split(/[\\/]/).filter(Boolean).pop() || p;
}

export default function App() {
  // Light Windows 11 Fluent Mica by default — synced style with DocPurger
  const [darkMode, setDarkMode] = useState<boolean>(() => {
    const saved = localStorage.getItem('dirlocker:fluentTheme');
    return saved ? saved === 'dark' : false;
  });

  useEffect(() => {
    document.documentElement.classList.toggle('dark', darkMode);
    localStorage.setItem('dirlocker:fluentTheme', darkMode ? 'dark' : 'light');
  }, [darkMode]);

  const [sysInfo, setSysInfo] = useState<SystemInfo | null>(null);
  const [status, setStatus] = useState<StatusResponse>({
    has_master_password: true,
    daemon_connected: true,
    hotkey_active: true,
    tray_available: false,
    settings: DEFAULT_SETTINGS,
    entries: [],
  });
  const [loading, setLoading] = useState(true);
  const [isMaximised, setIsMaximised] = useState(false);

  // Post-install wizard mode
  const [postInstallMode, setPostInstallMode] = useState(false);
  const [postInstallLaunch, setPostInstallLaunch] = useState(true);

  // Main window master password gate
  const [gateLocked, setGateLocked] = useState(false);
  const [gatePw, setGatePw] = useState('');
  const [gateErr, setGateErr] = useState('');
  const [gateBusy, setGateBusy] = useState(false);

  // Filter & Search
  const [filterTab, setFilterTab] = useState<FilterTab>('all');
  const [searchQuery, setSearchQuery] = useState('');

  // Modals
  const [masterModal, setMasterModal] = useState<{ open: boolean; isInitial: boolean }>({
    open: false,
    isInitial: false,
  });
  const [lockModalPaths, setLockModalPaths] = useState<string[]>([]);
  const [unlockTarget, setUnlockTarget] = useState<{ entry: DirEntry | null; fallbackPath?: string } | null>(
    null
  );
  const [changePwEntry, setChangePwEntry] = useState<DirEntry | null>(null);
  const [removeEntry, setRemoveEntry] = useState<DirEntry | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [healthOpen, setHealthOpen] = useState(false);
  const [recoveryOpen, setRecoveryOpen] = useState(false);
  const [aboutOpen, setAboutOpen] = useState(false);
  const [quitModalOpen, setQuitModalOpen] = useState(false);

  // Toast
  const [toast, setToast] = useState<ToastState | null>(null);
  const notify = useCallback((message: string, tone: NotifyTone = 'info') => {
    const id = Date.now() + Math.random();
    setToast({ id, message, tone });
    setTimeout(() => {
      setToast((prev) => (prev?.id === id ? null : prev));
    }, 3600);
  }, []);

  const refreshStatus = useCallback(async (checkInitial = false) => {
    try {
      const st = await backend.getStatus();
      setStatus(st);
      if (checkInitial && st.daemon_connected) {
        if (!st.has_master_password) {
          setMasterModal({ open: true, isInitial: true });
        } else if (st.settings.require_master_on_open) {
          setGateLocked(true);
        }
      }
      return st;
    } catch (e: any) {
      notify(String(e?.message || e || '加载状态失败'), 'error');
      return null;
    } finally {
      setLoading(false);
    }
  }, [notify]);

  useEffect(() => {
    backend
      .getSystemInfo()
      .then((info) => {
        setSysInfo(info);
        if (info.startMode === 'post-install') {
          setPostInstallMode(true);
        }
      })
      .catch(() => {});
    refreshStatus(true);
    backend.windowIsMaximised().then(setIsMaximised).catch(() => {});
    backend.syncWindowCorners();
    const onResize = () => {
      backend.windowIsMaximised().then(setIsMaximised).catch(() => {});
      backend.syncWindowCorners();
    };
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, [refreshStatus]);

  // Listen to backend push events
  useEffect(() => {
    const offState = backend.onEvent<StatusResponse>('dirlocker:state', (st) => {
      if (st) setStatus(st);
    });
    const offToast = backend.onEvent<{ message: string; tone?: NotifyTone }>('dirlocker:toast', (t) => {
      if (t?.message) notify(t.message, t.tone || 'info');
    });
    const offExitPostInstall = backend.onEvent<boolean>('dirlocker:exit-post-install', () => {
      setPostInstallMode(false);
      refreshStatus(true);
    });
    const offHidden = backend.onEvent<boolean>('dirlocker:hidden-to-tray', () => {
      setStatus((cur) => {
        if (cur.has_master_password && cur.settings.require_master_on_open) {
          setGateLocked(true);
          setGatePw('');
          setGateErr('');
        }
        return cur;
      });
    });
    const offAction = backend.onEvent<ActionRequestEvent>('dirlocker:action', async (act) => {
      if (!act) return;
      if (act.type === 'post-install') {
        setPostInstallMode(true);
        return;
      }
      if (act.type === 'quit-confirm') {
        setQuitModalOpen(true);
        return;
      }
      const st = await refreshStatus(false);
      if (!st) return;
      if (!st.has_master_password) {
        setMasterModal({ open: true, isInitial: true });
        return;
      }
      if (act.type === 'unlock') {
        const matched =
          st.entries.find((e) => act.entryId && e.id === act.entryId) ||
          st.entries.find(
            (e) => act.path && e.path.toLowerCase() === act.path.toLowerCase()
          ) ||
          null;
        if (matched && !matched.locked) {
          await backend.openFolder(matched.path);
          notify('目录已处于解锁状态，已为您打开', 'info');
          return;
        }
        setUnlockTarget({ entry: matched, fallbackPath: act.path });
      } else if (act.type === 'lock' && act.path) {
        const matched =
          st.entries.find((e) => act.entryId && e.id === act.entryId) ||
          st.entries.find(
            (e) => e.path.toLowerCase() === act.path!.toLowerCase()
          );
        if (matched) {
          if (matched.locked) {
            notify('该目录已处于锁定状态', 'info');
            return;
          }
          try {
            const next = await backend.lockDir(matched.id, matched.path);
            setStatus(next);
            notify(`已重新锁定：${leafName(matched.path)}`, 'success');
          } catch (e: any) {
            notify(String(e?.message || e || '锁定失败'), 'error');
          }
        } else {
          setLockModalPaths([act.path]);
        }
      }
    });
    return () => {
      offState();
      offToast();
      offExitPostInstall();
      offHidden();
      offAction();
    };
  }, [notify, refreshStatus]);

  // Native OS folder drag-and-drop
  useEffect(() => {
    return backend.onFileDrop(async (paths) => {
      if (!paths || paths.length === 0 || postInstallMode || gateLocked) return;
      if (!status.daemon_connected) {
        notify('后台服务未连接，暂时无法锁定目录', 'error');
        return;
      }
      if (!status.has_master_password) {
        setMasterModal({ open: true, isInitial: true });
        return;
      }
      const validDirs = await backend.filterDirectories(paths);
      if (validDirs.length === 0) {
        notify('请拖入文件夹（不支持直接拖入单个文件）', 'error');
        return;
      }
      setLockModalPaths(validDirs);
    }, true);
  }, [notify, postInstallMode, gateLocked, status.daemon_connected, status.has_master_password]);

  const handleToggleMaximise = useCallback(() => {
    backend.windowToggleMaximise();
    setTimeout(() => {
      backend.windowIsMaximised().then(setIsMaximised).catch(() => {});
      backend.syncWindowCorners();
    }, 80);
  }, []);

  const handleAddClick = async () => {
    if (!status.daemon_connected) {
      notify('后台服务未连接，请先启动 dirlockerd 服务', 'error');
      return;
    }
    if (!status.has_master_password) {
      setMasterModal({ open: true, isInitial: true });
      return;
    }
    const picked = await backend.selectDirectory(sysInfo?.homeDir || '');
    if (picked) {
      setLockModalPaths([picked]);
    }
  };

  const handleBatchAddAndLock = async (paths: string[], useCustom: boolean, customPw: string) => {
    let lastStatus: StatusResponse | null = null;
    let okCount = 0;
    const errors: string[] = [];
    for (const p of paths) {
      try {
        lastStatus = await backend.addAndLock(p, useCustom, customPw);
        okCount++;
      } catch (e: any) {
        errors.push(`${leafName(p)}：${e?.message || e}`);
      }
    }
    if (lastStatus) setStatus(lastStatus);
    if (errors.length > 0) {
      if (okCount > 0) {
        notify(`已锁定 ${okCount} 个目录，${errors.length} 个失败`, 'error');
      }
      throw new Error(errors.join('\n'));
    }
    notify(
      okCount > 1 ? `已成功锁定 ${okCount} 个目录` : `已锁定目录：${leafName(paths[0])}`,
      'success'
    );
  };

  const handleLockEntry = async (entry: DirEntry) => {
    try {
      const next = await backend.lockDir(entry.id, entry.path);
      setStatus(next);
      notify(`已锁定：${leafName(entry.path)}`, 'success');
    } catch (e: any) {
      notify(String(e?.message || e || '锁定失败'), 'error');
    }
  };

  const handleLockAll = async () => {
    try {
      const res = await backend.lockAll();
      await refreshStatus(false);
      if (res.failed_count > 0) {
        notify(
          `已锁定 ${res.locked_count} 个，另有 ${res.failed_count} 个被占用跳过：${res.failed_details?.[0] || ''}`,
          'error'
        );
      } else if (res.locked_count > 0) {
        notify(`一键全锁完成：已锁定 ${res.locked_count} 个目录`, 'success');
      } else {
        notify('当前所有受管目录均已处于锁定状态', 'info');
      }
    } catch (e: any) {
      notify(String(e?.message || e || '一键全锁失败'), 'error');
    }
  };

  const handleUnlockSubmit = async (id: string, path: string, pw: string, openAfter: boolean) => {
    const next = await backend.unlockDir(id, path, pw, openAfter);
    setStatus(next);
    notify(`已解锁：${leafName(path)}`, 'success');
  };

  const handleMasterSubmit = async (oldPw: string, newPw: string) => {
    const next = await backend.setMasterPassword(oldPw, newPw);
    setStatus(next);
    notify(masterModal.isInitial ? '全局主密码已设置，可以开始保护目录' : '全局主密码已更新', 'success');
  };

  const handleChangeDirPwSubmit = async (
    id: string,
    currentPw: string,
    useCustom: boolean,
    newCustomPw: string
  ) => {
    const next = await backend.changeDirPassword(id, currentPw, useCustom, newCustomPw);
    setStatus(next);
    notify(useCustom ? '已更新该目录的独立密码' : '已切换为使用全局主密码', 'success');
  };

  const handleRemoveSubmit = async (id: string, pw: string) => {
    const next = await backend.removeDir(id, pw);
    setStatus(next);
    notify('已解锁并将该目录移出保护列表（原文件完好）', 'success');
  };

  const handleSaveSettings = async (pw: string, nextSettings: Settings) => {
    const next = await backend.updateSettings(pw, nextSettings);
    setStatus(next);
    notify('安全设置与快捷键已保存', 'success');
  };

  const handleGateUnlock = async () => {
    setGateErr('');
    if (!gatePw) {
      setGateErr('请输入全局主密码');
      return;
    }
    setGateBusy(true);
    try {
      await backend.verifyMaster(gatePw);
      setGateLocked(false);
      setGatePw('');
    } catch (e: any) {
      setGateErr(String(e?.message || e || '主密码验证失败'));
    } finally {
      setGateBusy(false);
    }
  };

  const handleExplicitQuit = () => {
    if (counts.unlocked > 0) {
      setQuitModalOpen(true);
    } else {
      backend.forceQuit(false);
    }
  };

  const counts = useMemo(() => {
    let locked = 0;
    let unlocked = 0;
    let custom = 0;
    for (const e of status.entries) {
      if (e.locked) locked++;
      else unlocked++;
      if (e.use_custom_password) custom++;
    }
    return { all: status.entries.length, locked, unlocked, custom };
  }, [status.entries]);

  const visibleEntries = useMemo(() => {
    const q = searchQuery.trim().toLowerCase();
    return status.entries.filter((e) => {
      if (filterTab === 'locked' && !e.locked) return false;
      if (filterTab === 'unlocked' && e.locked) return false;
      if (filterTab === 'custom' && !e.use_custom_password) return false;
      if (q && !e.path.toLowerCase().includes(q) && !leafName(e.path).toLowerCase().includes(q)) {
        return false;
      }
      return true;
    });
  }, [status.entries, filterTab, searchQuery]);

  const copyPath = (p: string) => {
    navigator.clipboard?.writeText(p).then(() => notify('已复制目录路径', 'info')).catch(() => {});
  };

  // ==================== Post-Install Completion View (Linux .deb / Wizard) ====================
  if (postInstallMode) {
    return (
      <div className="mica-backdrop h-screen w-screen flex flex-col overflow-hidden text-slate-800 dark:text-slate-100 select-none rounded-[10px] border border-slate-300/65 dark:border-white/12">
        <div className="wails-drag h-10 shrink-0 px-3.5 flex items-center justify-between border-b border-slate-200/60 dark:border-white/[0.07] bg-white/40 dark:bg-black/20 backdrop-blur-md">
          <div className="flex items-center gap-2">
            <AppLogo className="w-5 h-5 shrink-0" />
            <span className="text-[13px] font-bold text-slate-900 dark:text-white">安装完成 — 目录密匣</span>
          </div>
          <button
            type="button"
            onClick={() => backend.completePostInstall(false)}
            className="wails-no-drag w-8 h-7 inline-flex items-center justify-center rounded-md text-slate-500 hover:bg-[#E81123] hover:text-white transition-colors"
            title="关闭"
          >
            <X className="w-3.5 h-3.5" />
          </button>
        </div>

        <div className="flex-1 p-5 flex flex-col justify-between">
          <div className="flex items-start gap-3.5">
            <AppLogo className="w-12 h-12 shrink-0 drop-shadow-md" />
            <div className="space-y-1.5">
              <h2 className="text-[15.5px] font-bold text-slate-900 dark:text-white">
                目录密匣 v{sysInfo?.appVersion || '3.0.0'} 已安装完成
              </h2>
              <p className="text-xs text-slate-600 dark:text-slate-300 leading-relaxed">
                系统右键菜单、加密目录图标及后台守护服务均已就绪。您可以随时从桌面菜单或文件管理器右键锁定目录。
              </p>
            </div>
          </div>

          <div className="space-y-4">
            <label className="flex items-center gap-2 text-[13px] font-medium text-slate-800 dark:text-slate-100 cursor-pointer select-none px-1">
              <input
                type="checkbox"
                checked={postInstallLaunch}
                onChange={(e) => setPostInstallLaunch(e.target.checked)}
                className="w-4 h-4 rounded accent-[#0067C0]"
              />
              <span>立即启动 目录密匣</span>
            </label>

            <div className="flex justify-end">
              <button
                type="button"
                onClick={() => {
                  if (postInstallLaunch) {
                    setPostInstallMode(false);
                    backend.completePostInstall(true);
                    refreshStatus(true);
                  } else {
                    backend.completePostInstall(false);
                  }
                }}
                className="px-6 py-2 rounded-lg text-xs font-semibold fluent-btn-primary"
              >
                完成
              </button>
            </div>
          </div>
        </div>
      </div>
    );
  }

  // ==================== Main Frameless Fluent Mica Window ====================
  return (
    <div
      className={`mica-backdrop h-screen w-screen flex flex-col overflow-hidden text-slate-800 dark:text-slate-100 select-none ${
        isMaximised ? '' : 'rounded-[10px] border border-slate-300/65 dark:border-white/12'
      }`}
    >
      {/* ================= INTEGRATED FRAMELESS FLUENT 2 TITLEBAR (100% matched to DocPurger) ================= */}
      <div
        onDoubleClick={handleToggleMaximise}
        className="wails-drag h-10 shrink-0 px-3.5 flex items-center justify-between border-b border-slate-200/60 dark:border-white/[0.07] bg-white/40 dark:bg-black/20 backdrop-blur-md"
      >
        <div className="flex items-center gap-2.5 min-w-0">
          <AppLogo className="w-6 h-6 shrink-0 drop-shadow-sm" />
          <div className="flex items-center gap-2 min-w-0">
            <span className="text-[13.5px] font-bold tracking-tight text-slate-900 dark:text-white">
              目录密匣
            </span>
            <span className="text-[11px] font-mono text-slate-400 dark:text-slate-400">
              DirLocker v{sysInfo?.appVersion || '3.0.0'}
            </span>
          </div>
        </div>

        <div className="wails-no-drag flex items-center gap-1">
          <button
            type="button"
            onClick={() => setDarkMode((v) => !v)}
            className="w-8 h-7 inline-flex items-center justify-center rounded-md text-slate-500 dark:text-slate-400 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/5 dark:hover:bg-white/10 transition-colors"
            title={darkMode ? '切换为浅色 Mica 云母' : '切换为深海云母暗色'}
          >
            {darkMode ? <Sun className="w-3.5 h-3.5" /> : <Moon className="w-3.5 h-3.5" />}
          </button>
          <span className="h-3.5 w-px bg-slate-300/70 dark:bg-white/15 mx-1" />
          <button
            type="button"
            onClick={() => backend.windowMinimise()}
            className="w-8 h-7 inline-flex items-center justify-center rounded-md text-slate-500 dark:text-slate-400 hover:text-slate-900 dark:hover:text-white hover:bg-black/5 dark:hover:bg-white/10 transition-colors"
            title="最小化"
          >
            <Minus className="w-3.5 h-3.5" />
          </button>
          <button
            type="button"
            onClick={handleToggleMaximise}
            className="w-8 h-7 inline-flex items-center justify-center rounded-md text-slate-500 dark:text-slate-400 hover:text-slate-900 dark:hover:text-white hover:bg-black/5 dark:hover:bg-white/10 transition-colors"
            title={isMaximised ? '向下还原' : '最大化'}
          >
            {isMaximised ? <Minimize2 className="w-3.5 h-3.5" /> : <Maximize2 className="w-3.5 h-3.5" />}
          </button>
          <button
            type="button"
            onClick={() => backend.hideToTrayOrQuit()}
            className="w-8 h-7 inline-flex items-center justify-center rounded-md text-slate-500 dark:text-slate-400 hover:bg-[#E81123] hover:text-white transition-colors"
            title={status.tray_available ? '最小化到系统托盘' : '关闭'}
          >
            <X className="w-3.5 h-3.5" />
          </button>
        </div>
      </div>

      {/* ================= SLEEK SINGLE-COLUMN FLUENT MICA WORKSPACE ================= */}
      <div className="flex-1 min-h-0 flex flex-col px-4 pt-3 pb-3 gap-3 overflow-hidden relative">
        {/* Top Elevated Fluent Command & Search Bar */}
        <div className="fluent-search-bar rounded-xl px-3 py-2 flex items-center gap-2.5 shrink-0">
          <button
            type="button"
            onClick={handleAddClick}
            className="inline-flex items-center gap-1.5 px-3.5 py-2 rounded-lg text-[12.5px] font-medium fluent-btn-primary whitespace-nowrap shrink-0"
          >
            <FolderPlus className="w-4 h-4 shrink-0" />
            添加并锁定
          </button>

          <button
            type="button"
            onClick={handleLockAll}
            disabled={counts.unlocked === 0}
            className="inline-flex items-center gap-1.5 px-3 py-2 rounded-lg text-[12.5px] font-medium fluent-btn-subtle whitespace-nowrap shrink-0 disabled:opacity-45"
            title={`立即锁定全部已解锁目录（快捷键 ${status.settings.hotkey || 'Ctrl+Alt+K'}）`}
          >
            <Lock className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF] shrink-0" />
            <span>一键全锁</span>
            <kbd className="px-1.5 py-[1px] rounded text-[10.5px] font-mono bg-slate-200/70 dark:bg-white/10 text-slate-600 dark:text-slate-300">
              {status.settings.hotkey || 'Ctrl+Alt+K'}
            </kbd>
          </button>

          {/* Search Input */}
          <div className="flex-1 min-w-[160px] flex items-center gap-2 px-2.5 py-1.5 rounded-lg bg-slate-100/75 dark:bg-black/25 border border-slate-200/70 dark:border-white/[0.08]">
            <Search className="w-3.5 h-3.5 text-slate-400 shrink-0" />
            <input
              type="text"
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              placeholder="搜索目录名称或路径…"
              className="flex-1 min-w-0 bg-transparent text-[12.5px] text-slate-800 dark:text-slate-100 placeholder:text-slate-400 outline-none"
            />
            {searchQuery && (
              <button
                type="button"
                onClick={() => setSearchQuery('')}
                className="p-0.5 rounded text-slate-400 hover:text-slate-600 dark:hover:text-slate-200"
              >
                <X className="w-3 h-3" />
              </button>
            )}
          </div>

          <span className="h-5 w-px bg-slate-200/80 dark:bg-white/10 shrink-0" />

          {/* Clean Compact Utility Actions */}
          <div className="flex items-center gap-1 shrink-0">
            <button
              type="button"
              onClick={() => setMasterModal({ open: true, isInitial: !status.has_master_password })}
              className="inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs text-slate-600 dark:text-slate-300 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.04] dark:hover:bg-white/[0.07] transition-colors whitespace-nowrap"
              title="修改全局主密码"
            >
              <KeyRound className="w-3.5 h-3.5" />
              <span>主密码</span>
            </button>
            <button
              type="button"
              onClick={() => setSettingsOpen(true)}
              className="inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs text-slate-600 dark:text-slate-300 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.04] dark:hover:bg-white/[0.07] transition-colors whitespace-nowrap"
              title="安全设置与快捷键"
            >
              <SettingsIcon className="w-3.5 h-3.5" />
              <span>设置</span>
            </button>
            <button
              type="button"
              onClick={() => setHealthOpen(true)}
              className="inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs text-slate-600 dark:text-slate-300 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.04] dark:hover:bg-white/[0.07] transition-colors whitespace-nowrap"
              title="保护区体检与一键修复"
            >
              <Activity className="w-3.5 h-3.5" />
              <span>体检</span>
            </button>
            <button
              type="button"
              onClick={() => setRecoveryOpen(true)}
              className="inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs text-slate-600 dark:text-slate-300 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.04] dark:hover:bg-white/[0.07] transition-colors whitespace-nowrap"
              title="应急恢复说明"
            >
              <LifeBuoy className="w-3.5 h-3.5" />
              <span>恢复</span>
            </button>
            <button
              type="button"
              onClick={() => setAboutOpen(true)}
              className="p-1.5 rounded-lg text-slate-500 dark:text-slate-400 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.04] dark:hover:bg-white/[0.07] transition-colors"
              title="关于目录密匣"
            >
              <Info className="w-4 h-4" />
            </button>
          </div>
        </div>

        {/* Warning Banners (only shown when relevant) */}
        {!status.daemon_connected && (
          <div className="rounded-xl px-3.5 py-2.5 bg-rose-500/12 border border-rose-500/25 flex items-center justify-between gap-3 text-xs text-rose-700 dark:text-rose-200 shrink-0">
            <div className="flex items-center gap-2 min-w-0">
              <AlertTriangle className="w-4 h-4 shrink-0" />
              <span className="truncate">{status.daemon_error || '无法连接后台服务 dirlockerd，请检查服务是否启动'}</span>
            </div>
            <button
              type="button"
              onClick={() => refreshStatus(false)}
              className="inline-flex items-center gap-1 px-2.5 py-1 rounded-md bg-white/80 dark:bg-white/10 text-xs font-medium shrink-0"
            >
              <RefreshCw className="w-3 h-3" />
              重试
            </button>
          </div>
        )}

        {status.daemon_connected && !status.hotkey_active && status.hotkey_warning && (
          <div className="rounded-xl px-3.5 py-2 bg-amber-500/10 border border-amber-500/25 flex items-center justify-between gap-3 text-xs text-amber-800 dark:text-amber-200 shrink-0">
            <div className="flex items-center gap-2 min-w-0">
              <AlertTriangle className="w-3.5 h-3.5 shrink-0 text-amber-600 dark:text-amber-400" />
              <span className="truncate">{status.hotkey_warning}</span>
            </div>
            <button
              type="button"
              onClick={() => setSettingsOpen(true)}
              className="text-[#0067C0] dark:text-[#60CDFF] font-medium hover:underline shrink-0"
            >
              更换快捷键
            </button>
          </div>
        )}

        {/* Main Layered Mica Directory List Container (Native Drag & Drop Target) */}
        <div className="mica-panel dir-drop-zone flex-1 min-h-0 rounded-2xl flex flex-col overflow-hidden transition-all">
          {/* Panel Header & Compact Filter Tabs */}
          <div className="px-4 py-2.5 border-b border-slate-200/60 dark:border-white/[0.07] flex items-center justify-between gap-3 shrink-0">
            <div className="flex items-center gap-1.5">
              {(
                [
                  { id: 'all', label: '全部目录', count: counts.all },
                  { id: 'locked', label: '已锁定', count: counts.locked },
                  { id: 'unlocked', label: '已解锁', count: counts.unlocked },
                  { id: 'custom', label: '独立密码', count: counts.custom },
                ] as { id: FilterTab; label: string; count: number }[]
              ).map((tab) => {
                const active = filterTab === tab.id;
                return (
                  <button
                    key={tab.id}
                    type="button"
                    onClick={() => setFilterTab(tab.id)}
                    className={`inline-flex items-center gap-1.5 px-2.5 py-1 rounded-lg text-xs transition-all ${
                      active
                        ? 'bg-[#0067C0] text-white dark:bg-[#60CDFF] dark:text-[#0A1929] font-semibold shadow-sm'
                        : 'text-slate-600 dark:text-slate-300 hover:bg-black/[0.04] dark:hover:bg-white/[0.07]'
                    }`}
                  >
                    <span>{tab.label}</span>
                    <span
                      className={`px-1.5 py-[0.5px] rounded-full text-[10.5px] font-mono ${
                        active
                          ? 'bg-white/25 dark:bg-black/15'
                          : 'bg-slate-200/75 dark:bg-white/10 text-slate-500 dark:text-slate-400'
                      }`}
                    >
                      {tab.count}
                    </span>
                  </button>
                );
              })}
            </div>

            <div className="flex items-center gap-2 text-[11.5px] text-slate-400 dark:text-slate-400">
              <span>支持拖入文件夹直接上锁</span>
              <button
                type="button"
                onClick={() => refreshStatus(false)}
                className="p-1 rounded-md hover:bg-black/[0.05] dark:hover:bg-white/10 text-slate-500 dark:text-slate-400 transition-colors"
                title="刷新目录状态"
              >
                <RefreshCw className="w-3.5 h-3.5" />
              </button>
            </div>
          </div>

          {/* Directory List / Empty State */}
          <div className="flex-1 min-h-0 overflow-y-auto p-3.5">
            {loading ? (
              <div className="h-full flex items-center justify-center text-xs text-slate-400">
                正在读取受管目录…
              </div>
            ) : visibleEntries.length === 0 ? (
              <div className="h-full flex items-center justify-center">
                <button
                  type="button"
                  onClick={handleAddClick}
                  className="group max-w-[420px] w-full rounded-2xl border border-dashed border-slate-300/90 dark:border-white/15 hover:border-[#0067C0] dark:hover:border-[#60CDFF] bg-white/55 dark:bg-white/[0.03] hover:bg-white/80 dark:hover:bg-white/[0.06] px-8 py-9 flex flex-col items-center text-center gap-2.5 transition-all"
                >
                  <LockedFolderIcon className="w-14 h-14 drop-shadow-sm group-hover:scale-105 transition-transform" />
                  <div className="text-[14px] font-semibold text-slate-800 dark:text-slate-100 mt-1">
                    {status.entries.length === 0
                      ? '将要保护的目录拖放到此处'
                      : '当前筛选条件下暂无匹配目录'}
                  </div>
                  <div className="text-xs text-slate-500 dark:text-slate-400 leading-relaxed">
                    {status.entries.length === 0
                      ? '或点击此处 / 左上角「添加并锁定」，原目录内文件绝不被修改或加密'
                      : '可切换上方状态标签或清空搜索关键词查看全部受管目录'}
                  </div>
                </button>
              </div>
            ) : (
              <div className="space-y-2">
                {visibleEntries.map((entry) => (
                  <div
                    key={entry.id}
                    className="fluent-result-card rounded-xl px-3.5 py-2.5 flex items-center justify-between gap-3"
                  >
                    {/* Left: Fluent Folder Icon + Name & Path */}
                    <div className="flex items-center gap-3 min-w-0 flex-1">
                      {entry.locked ? (
                        <LockedFolderIcon className="w-9 h-9 shrink-0 drop-shadow-sm" />
                      ) : (
                        <UnlockedFolderIcon className="w-9 h-9 shrink-0 drop-shadow-sm" />
                      )}
                      <div className="min-w-0 flex-1">
                        <div className="flex items-center gap-2 min-w-0">
                          <span className="text-[13.5px] font-semibold text-slate-900 dark:text-white truncate">
                            {leafName(entry.path)}
                          </span>
                          {/* Status Badge */}
                          {entry.locked ? (
                            <span className="inline-flex items-center gap-1 px-2 py-[1.5px] rounded-full text-[11px] font-semibold bg-[#0067C0]/12 text-[#0067C0] dark:bg-[#60CDFF]/15 dark:text-[#60CDFF] shrink-0">
                              <Lock className="w-2.5 h-2.5" />
                              {entry.in_place_acl ? '原位锁定' : '已锁定'}
                            </span>
                          ) : (
                            <span className="inline-flex items-center gap-1 px-2 py-[1.5px] rounded-full text-[11px] font-semibold bg-emerald-500/12 text-emerald-700 dark:bg-emerald-400/15 dark:text-emerald-300 shrink-0">
                              <CheckCircle2 className="w-2.5 h-2.5" />
                              已解锁
                            </span>
                          )}

                          {/* Password Mode Chip (clickable to edit) */}
                          <button
                            type="button"
                            onClick={() => setChangePwEntry(entry)}
                            className={`inline-flex items-center gap-1 px-2 py-[1.5px] rounded-full text-[11px] transition-colors shrink-0 ${
                              entry.use_custom_password
                                ? 'bg-violet-500/12 text-violet-700 dark:bg-violet-400/15 dark:text-violet-300 hover:bg-violet-500/20'
                                : 'bg-slate-200/70 text-slate-600 dark:bg-white/10 dark:text-slate-300 hover:bg-slate-300/70 dark:hover:bg-white/15'
                            }`}
                            title="点击修改该目录的密码方式"
                          >
                            <KeyRound className="w-2.5 h-2.5" />
                            {entry.use_custom_password ? '独立密码' : '全局主密码'}
                          </button>
                        </div>

                        <div className="flex items-center gap-1.5 mt-0.5 min-w-0">
                          <span
                            className="text-[11.5px] font-mono text-slate-500 dark:text-slate-400 truncate"
                            title={entry.path}
                          >
                            {entry.path}
                          </span>
                          <button
                            type="button"
                            onClick={() => copyPath(entry.path)}
                            className="p-0.5 rounded text-slate-400 hover:text-[#0067C0] dark:hover:text-[#60CDFF] shrink-0"
                            title="复制完整路径"
                          >
                            <Copy className="w-3 h-3" />
                          </button>
                        </div>
                      </div>
                    </div>

                    {/* Right: Row Action Controls */}
                    <div className="flex items-center gap-1.5 shrink-0">
                      {entry.locked ? (
                        <button
                          type="button"
                          onClick={() => setUnlockTarget({ entry })}
                          className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
                        >
                          <Unlock className="w-3.5 h-3.5" />
                          解锁
                        </button>
                      ) : (
                        <>
                          <button
                            type="button"
                            onClick={() => backend.openFolder(entry.path)}
                            className="inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
                            title="在文件管理器中打开"
                          >
                            <FolderOpen className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                            打开
                          </button>
                          <button
                            type="button"
                            onClick={() => handleLockEntry(entry)}
                            className="inline-flex items-center gap-1 px-3 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
                          >
                            <Lock className="w-3.5 h-3.5" />
                            锁定
                          </button>
                        </>
                      )}

                      <button
                        type="button"
                        onClick={() => setChangePwEntry(entry)}
                        className="p-1.5 rounded-lg text-slate-500 dark:text-slate-400 hover:text-[#0067C0] dark:hover:text-[#60CDFF] hover:bg-black/[0.05] dark:hover:bg-white/10 transition-colors"
                        title="修改目录密码方式"
                      >
                        <KeyRound className="w-3.5 h-3.5" />
                      </button>
                      <button
                        type="button"
                        onClick={() => setRemoveEntry(entry)}
                        className="p-1.5 rounded-lg text-slate-500 dark:text-slate-400 hover:text-rose-600 dark:hover:text-rose-400 hover:bg-rose-500/10 transition-colors"
                        title="解锁并移出保护列表"
                      >
                        <Trash2 className="w-3.5 h-3.5" />
                      </button>
                    </div>
                  </div>
                ))}
              </div>
            )}
          </div>
        </div>

        {/* Bottom Compact Fluent Status & Policy Strip */}
        <div className="acrylic-surface rounded-xl px-3.5 py-2 flex items-center justify-between gap-3 text-[11.5px] text-slate-500 dark:text-slate-400 shrink-0">
          <div className="flex items-center gap-3 min-w-0 flex-wrap">
            <span className="inline-flex items-center gap-1.5 text-slate-700 dark:text-slate-200 font-medium">
              <ShieldCheck className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400" />
              零加密无损保护
            </span>
            <span className="text-slate-300 dark:text-white/15">|</span>
            <span>
              闲置回锁：
              <strong className="font-medium text-slate-700 dark:text-slate-200">
                {status.settings.auto_relock_minutes > 0
                  ? `${status.settings.auto_relock_minutes} 分钟`
                  : '已关闭'}
              </strong>
            </span>
            <span className="text-slate-300 dark:text-white/15">|</span>
            <span>
              一键全锁：
              <code className="font-mono text-[#0067C0] dark:text-[#60CDFF]">
                {status.settings.hotkey || 'Ctrl+Alt+K'}
              </code>
            </span>
            {sysInfo?.os === 'windows' && (
              <>
                <span className="text-slate-300 dark:text-white/15">|</span>
                <span>
                  占用策略：
                  <strong className="font-medium text-slate-700 dark:text-slate-200">
                    {status.settings.allow_in_place_fallback ? '原位降级自动升级' : '防占用拦截'}
                  </strong>
                </span>
              </>
            )}
          </div>

          <div className="flex items-center gap-2 shrink-0">
            <button
              type="button"
              onClick={handleExplicitQuit}
              className="inline-flex items-center gap-1 px-2 py-0.5 rounded-md text-[11px] text-slate-500 dark:text-slate-400 hover:text-rose-600 dark:hover:text-rose-400 hover:bg-rose-500/10 transition-colors"
              title="彻底退出目录密匣（不留驻后台托盘）"
            >
              <Power className="w-3 h-3" />
              退出程序
            </button>
          </div>
        </div>

        {/* Master Password Window Gate Overlay */}
        {gateLocked && (
          <div className="absolute inset-0 z-40 flex items-center justify-center p-6 bg-slate-100/75 dark:bg-[#141820]/85 backdrop-blur-xl animate-fade-in">
            <div className="w-full max-w-[380px] rounded-2xl acrylic-surface p-6 space-y-4 text-center animate-modal-in">
              <div className="w-12 h-12 rounded-2xl bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center mx-auto">
                <Shield className="w-6 h-6" />
              </div>
              <div>
                <h3 className="text-base font-bold text-slate-900 dark:text-white">主窗口已锁定</h3>
                <p className="text-xs text-slate-500 dark:text-slate-400 mt-1">
                  已开启「打开主窗口验证主密码」，请输入全局主密码继续
                </p>
              </div>
              <PasswordInput
                value={gatePw}
                onChange={setGatePw}
                placeholder="请输入全局主密码"
                autoFocus
                disabled={gateBusy}
                onEnter={handleGateUnlock}
              />
              {gateErr && (
                <div className="px-3 py-2 rounded-lg text-xs text-left bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
                  {gateErr}
                </div>
              )}
              <button
                type="button"
                onClick={handleGateUnlock}
                disabled={gateBusy}
                className="w-full py-2 rounded-lg text-xs font-semibold fluent-btn-primary"
              >
                {gateBusy ? '验证中…' : '验证并打开主窗口'}
              </button>
            </div>
          </div>
        )}
      </div>

      {/* ================= MODALS ================= */}
      <MasterPasswordModal
        open={masterModal.open}
        isInitial={masterModal.isInitial}
        onClose={() => setMasterModal({ open: false, isInitial: false })}
        onSubmit={handleMasterSubmit}
      />

      <LockDirModal
        open={lockModalPaths.length > 0}
        paths={lockModalPaths}
        onClose={() => setLockModalPaths([])}
        onSubmit={handleBatchAddAndLock}
      />

      <UnlockDirModal
        open={Boolean(unlockTarget)}
        entry={unlockTarget?.entry || null}
        fallbackPath={unlockTarget?.fallbackPath}
        onClose={() => setUnlockTarget(null)}
        onSubmit={handleUnlockSubmit}
      />

      <ChangeDirPasswordModal
        open={Boolean(changePwEntry)}
        entry={changePwEntry}
        onClose={() => setChangePwEntry(null)}
        onSubmit={handleChangeDirPwSubmit}
      />

      <RemoveDirModal
        open={Boolean(removeEntry)}
        entry={removeEntry}
        onClose={() => setRemoveEntry(null)}
        onSubmit={handleRemoveSubmit}
      />

      <SettingsModal
        open={settingsOpen}
        settings={status.settings}
        sysInfo={sysInfo}
        onClose={() => setSettingsOpen(false)}
        onSave={handleSaveSettings}
      />

      <HealthCheckModal
        open={healthOpen}
        sysInfo={sysInfo}
        onClose={() => setHealthOpen(false)}
        onRepaired={() => refreshStatus(false)}
        onNotify={notify}
      />

      <RecoveryModal
        open={recoveryOpen}
        sysInfo={sysInfo}
        onClose={() => setRecoveryOpen(false)}
        onNotify={notify}
      />

      <AboutModal
        open={aboutOpen}
        sysInfo={sysInfo}
        hotkey={status.settings.hotkey || 'Ctrl+Alt+K'}
        onClose={() => setAboutOpen(false)}
        onNotify={notify}
      />

      <QuitConfirmModal
        open={quitModalOpen}
        unlockedCount={counts.unlocked}
        onClose={() => setQuitModalOpen(false)}
        onRelockAndQuit={() => {
          setQuitModalOpen(false);
          backend.forceQuit(true);
        }}
        onDirectQuit={() => {
          setQuitModalOpen(false);
          backend.forceQuit(false);
        }}
      />

      {/* ================= FLOATING FLUENT TOAST ================= */}
      {toast && (
        <div className="fixed bottom-14 left-1/2 -translate-x-1/2 z-50 animate-dock-in pointer-events-none">
          <div
            className={`px-4 py-2 rounded-xl text-xs font-medium shadow-lg border backdrop-blur-xl flex items-center gap-2 ${
              toast.tone === 'error'
                ? 'bg-rose-600/95 text-white border-rose-400/40'
                : toast.tone === 'success'
                ? 'bg-emerald-600/95 text-white border-emerald-400/40'
                : 'bg-slate-900/90 dark:bg-slate-100/95 text-white dark:text-slate-900 border-white/15'
            }`}
          >
            {toast.tone === 'error' ? (
              <AlertTriangle className="w-3.5 h-3.5 shrink-0" />
            ) : toast.tone === 'success' ? (
              <CheckCircle2 className="w-3.5 h-3.5 shrink-0" />
            ) : (
              <Info className="w-3.5 h-3.5 shrink-0" />
            )}
            <span>{toast.message}</span>
          </div>
        </div>
      )}
    </div>
  );
}
