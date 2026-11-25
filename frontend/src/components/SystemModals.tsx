import React, { useEffect, useState } from 'react';
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  Copy,
  Keyboard,
  LifeBuoy,
  Lock,
  Power,
  RefreshCw,
  Settings as SettingsIcon,
  ShieldCheck,
  Wrench,
  X,
} from 'lucide-react';
import type { HealthReport, NotifyTone, Settings, SystemInfo } from '../types';
import { backend } from '../lib/wails';
import { FluentToggle, PasswordInput } from './FluentIcons';

/* ==================== 1. Security Settings Modal ==================== */
export const SettingsModal: React.FC<{
  open: boolean;
  settings: Settings;
  sysInfo: SystemInfo | null;
  onClose: () => void;
  onSave: (password: string, next: Settings) => Promise<void>;
}> = ({ open, settings, sysInfo, onClose, onSave }) => {
  const [autoMins, setAutoMins] = useState(settings.auto_relock_minutes);
  const [requireMaster, setRequireMaster] = useState(settings.require_master_on_open);
  const [allowFallback, setAllowFallback] = useState(settings.allow_in_place_fallback);
  const [hotkey, setHotkey] = useState(settings.hotkey || 'Ctrl+Alt+K');
  const [recording, setRecording] = useState(false);
  const [hkProbe, setHkProbe] = useState<{ ok: boolean; message: string } | null>(null);
  const [password, setPassword] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setAutoMins(settings.auto_relock_minutes);
      setRequireMaster(settings.require_master_on_open);
      setAllowFallback(settings.allow_in_place_fallback);
      setHotkey(settings.hotkey || 'Ctrl+Alt+K');
      setRecording(false);
      setHkProbe(null);
      setPassword('');
      setErr('');
      setBusy(false);
    }
  }, [open, settings]);

  if (!open) return null;

  const handleRecordKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>) => {
    if (!recording) return;
    e.preventDefault();
    e.stopPropagation();
    const k = e.key;
    if (k === 'Escape') {
      setRecording(false);
      return;
    }
    if (k === 'Control' || k === 'Alt' || k === 'Shift' || k === 'Meta') return;
    if (!e.ctrlKey && !e.altKey) {
      setHkProbe({ ok: false, message: '全局快捷键必须至少包含 Ctrl 或 Alt 修饰键' });
      return;
    }
    let mainKey = '';
    if (/^[a-zA-Z0-9]$/.test(k)) {
      mainKey = k.toUpperCase();
    } else if (/^F([1-9]|1[0-2])$/i.test(k)) {
      mainKey = k.toUpperCase();
    } else {
      setHkProbe({ ok: false, message: '主键仅支持 A-Z、0-9 或 F1-F12' });
      return;
    }
    const parts: string[] = [];
    if (e.ctrlKey) parts.push('Ctrl');
    if (e.altKey) parts.push('Alt');
    if (e.shiftKey) parts.push('Shift');
    parts.push(mainKey);
    const combo = parts.join('+');
    if (combo === 'Alt+F4') {
      setHkProbe({ ok: false, message: 'Alt+F4 为系统关闭窗口快捷键，请更换组合' });
      return;
    }
    setHotkey(combo);
    setRecording(false);
    backend.testHotkey(combo).then(setHkProbe).catch(() => {});
  };

  const handleProbe = async () => {
    const res = await backend.testHotkey(hotkey);
    setHkProbe(res);
  };

  const handleSubmit = async () => {
    setErr('');
    if (!password) {
      setErr('保存安全设置需要验证全局主密码');
      return;
    }
    setBusy(true);
    try {
      await onSave(password, {
        auto_relock_minutes: Math.max(0, Math.min(1440, Number(autoMins) || 0)),
        require_master_on_open: requireMaster,
        allow_in_place_fallback: allowFallback,
        hotkey,
      });
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '保存设置失败'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[520px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center">
              <SettingsIcon className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">安全设置与全局快捷键</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                配置一键全锁快捷键、闲置自动回锁策略与保护模式
              </p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="px-5 py-4 space-y-4 text-xs max-h-[72vh] overflow-y-auto">
          {/* Global Hotkey */}
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-3.5 space-y-2.5">
            <div className="flex items-center justify-between gap-2">
              <div>
                <div className="font-semibold text-slate-800 dark:text-slate-100 flex items-center gap-1.5">
                  <Keyboard className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                  一键全锁全局快捷键
                </div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  任意界面按下立即锁定所有已解锁目录（默认 Ctrl+Alt+K）
                </div>
              </div>
              <button
                type="button"
                onClick={() => {
                  setHotkey('Ctrl+Alt+K');
                  setRecording(false);
                  backend.testHotkey('Ctrl+Alt+K').then(setHkProbe).catch(() => {});
                }}
                className="text-[11px] text-[#0067C0] dark:text-[#60CDFF] hover:underline shrink-0"
              >
                恢复默认
              </button>
            </div>
            <div className="flex items-center gap-2">
              <button
                type="button"
                onClick={() => setRecording(true)}
                onKeyDown={handleRecordKeyDown}
                onBlur={() => setRecording(false)}
                className={`flex-1 px-3 py-2 rounded-lg font-mono text-xs text-left border transition-all ${
                  recording
                    ? 'bg-[#0067C0]/10 border-[#0067C0] text-[#0067C0] dark:bg-[#60CDFF]/15 dark:border-[#60CDFF] dark:text-[#60CDFF]'
                    : 'bg-white/90 dark:bg-black/25 border-slate-200 dark:border-white/15 text-slate-800 dark:text-slate-100'
                }`}
              >
                {recording ? '请按下新的组合键（如 Ctrl+Alt+K，Esc 取消）…' : hotkey}
              </button>
              <button
                type="button"
                onClick={handleProbe}
                className="px-3 py-2 rounded-lg text-xs font-medium fluent-btn-subtle shrink-0"
              >
                冲突检测
              </button>
            </div>
            {hkProbe && (
              <div
                className={`text-[11px] px-2.5 py-1.5 rounded-lg ${
                  hkProbe.ok
                    ? 'bg-emerald-500/10 text-emerald-700 dark:text-emerald-300'
                    : 'bg-rose-500/10 text-rose-600 dark:text-rose-300'
                }`}
              >
                {hkProbe.message}
              </div>
            )}
          </div>

          {/* Idle Auto-Relock */}
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-3.5 space-y-2.5">
            <div className="flex items-center justify-between gap-3">
              <div>
                <div className="font-semibold text-slate-800 dark:text-slate-100">系统闲置自动回锁</div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  无键盘鼠标操作达到设定分钟数后自动锁定（0 表示关闭）
                </div>
              </div>
              <div className="flex items-center gap-1.5 shrink-0">
                <input
                  type="number"
                  min={0}
                  max={1440}
                  value={autoMins}
                  onChange={(e) => setAutoMins(Math.max(0, Math.min(1440, parseInt(e.target.value, 10) || 0)))}
                  className="fluent-input w-20 rounded-lg px-2.5 py-1.5 text-xs font-mono text-center"
                />
                <span className="text-slate-500 dark:text-slate-400">分钟</span>
              </div>
            </div>
            <div className="flex items-center gap-1.5">
              {[
                { label: '关闭', val: 0 },
                { label: '5 分钟', val: 5 },
                { label: '15 分钟', val: 15 },
                { label: '30 分钟', val: 30 },
              ].map((p) => (
                <button
                  key={p.val}
                  type="button"
                  onClick={() => setAutoMins(p.val)}
                  className={`px-2.5 py-1 rounded-md text-[11px] transition-colors ${
                    autoMins === p.val
                      ? 'bg-[#0067C0]/15 text-[#0067C0] dark:bg-[#60CDFF]/20 dark:text-[#60CDFF] font-semibold'
                      : 'bg-slate-100/80 dark:bg-white/[0.06] text-slate-600 dark:text-slate-300 hover:bg-slate-200/70 dark:hover:bg-white/10'
                  }`}
                >
                  {p.label}
                </button>
              ))}
            </div>
          </div>

          {/* Toggles */}
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] divide-y divide-slate-200/60 dark:divide-white/[0.06]">
            <div className="px-3.5 py-3 flex items-center justify-between gap-3">
              <div>
                <div className="font-semibold text-slate-800 dark:text-slate-100">打开主窗口时验证主密码</div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  防止他人查看受保护目录列表及路径信息
                </div>
              </div>
              <FluentToggle checked={requireMaster} onChange={setRequireMaster} />
            </div>

            {sysInfo?.os === 'windows' && (
              <div className="px-3.5 py-3 flex items-center justify-between gap-3">
                <div>
                  <div className="font-semibold text-slate-800 dark:text-slate-100">
                    被占用时允许原位降级保护（实验）
                  </div>
                  <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                    默认关闭：检测到文档占用时安全阻止上锁；开启后先原位锁定，空闲后自动升级为保护区
                  </div>
                </div>
                <FluentToggle checked={allowFallback} onChange={setAllowFallback} />
              </div>
            )}
          </div>

          {/* Master Password Verification */}
          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">
              验证全局主密码以保存设置
            </label>
            <PasswordInput
              value={password}
              onChange={setPassword}
              placeholder="请输入全局主密码"
              disabled={busy}
              onEnter={handleSubmit}
            />
          </div>

          {err && (
            <div className="px-3 py-2 rounded-lg text-xs bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
              {err}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex justify-end gap-2">
          <button type="button" onClick={onClose} disabled={busy} className="px-3.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle">
            取消
          </button>
          <button type="button" onClick={handleSubmit} disabled={busy} className="px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary">
            {busy ? '保存中…' : '保存设置'}
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 2. Health Check & Safe Repair Modal ==================== */
export const HealthCheckModal: React.FC<{
  open: boolean;
  sysInfo: SystemInfo | null;
  onClose: () => void;
  onRepaired: () => void;
  onNotify: (msg: string, tone?: NotifyTone) => void;
}> = ({ open, sysInfo, onClose, onRepaired, onNotify }) => {
  const isLinux = sysInfo?.os === 'linux';
  const [password, setPassword] = useState('');
  const [report, setReport] = useState<HealthReport | null>(null);
  const [cleanDead, setCleanDead] = useState(false);
  const [repairLog, setRepairLog] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const [err, setErr] = useState('');

  const runScan = async (pw: string) => {
    setErr('');
    setLoading(true);
    try {
      const rep = await backend.diagnose(pw);
      setReport(rep);
    } catch (e: any) {
      setErr(String(e?.message || e || '扫描失败'));
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    if (open) {
      setPassword('');
      setReport(null);
      setCleanDead(false);
      setRepairLog([]);
      setErr('');
      if (!isLinux) {
        runScan('');
      }
    }
  }, [open, isLinux]);

  if (!open) return null;

  const hasDeadRecords = report?.items.some((i) => i.allow_remove_record) ?? false;
  const canRepair = (report?.repairable_count ?? 0) > 0 || (cleanDead && hasDeadRecords);

  const handleRepair = async () => {
    setErr('');
    setLoading(true);
    try {
      const res = await backend.repair(password, cleanDead);
      setRepairLog(res.details || []);
      onNotify(`安全修复完成，共处理 ${res.repaired_count} 项`, 'success');
      onRepaired();
      await runScan(password);
    } catch (e: any) {
      setErr(String(e?.message || e || '修复失败'));
    } finally {
      setLoading(false);
    }
  };

  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[580px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center">
              <Activity className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">保护区体检与一键修复</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                检测入口丢失、权限异常、残留文件与孤立保护区，绝不删除任何用户数据
              </p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="px-5 py-4 space-y-3.5 text-xs max-h-[68vh] overflow-y-auto">
          {isLinux && !report && (
            <div className="space-y-2.5">
              <div className="text-slate-600 dark:text-slate-300">
                银河麒麟后台服务 <code className="font-mono">dirlockerd</code> 需要验证全局主密码后执行保护区体检：
              </div>
              <div className="flex gap-2">
                <PasswordInput
                  value={password}
                  onChange={setPassword}
                  placeholder="请输入全局主密码"
                  autoFocus
                  disabled={loading}
                  onEnter={() => runScan(password)}
                  className="flex-1"
                />
                <button
                  type="button"
                  onClick={() => runScan(password)}
                  disabled={loading || !password}
                  className="px-4 py-2 rounded-lg text-xs font-medium fluent-btn-primary shrink-0"
                >
                  {loading ? '扫描中…' : '开始体检'}
                </button>
              </div>
            </div>
          )}

          {loading && !report && !isLinux && (
            <div className="py-10 text-center text-slate-500 dark:text-slate-400">正在全面检测保护区与状态记录…</div>
          )}

          {report && (
            <>
              <div className="grid grid-cols-4 gap-2">
                <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-2.5 text-center">
                  <div className="text-base font-bold text-slate-900 dark:text-white">{report.total_entries}</div>
                  <div className="text-[11px] text-slate-500">受管目录</div>
                </div>
                <div className="rounded-xl bg-emerald-500/10 border border-emerald-500/20 p-2.5 text-center">
                  <div className="text-base font-bold text-emerald-700 dark:text-emerald-300">{report.ok_count}</div>
                  <div className="text-[11px] text-emerald-700/80 dark:text-emerald-300/80">状态健康</div>
                </div>
                <div className="rounded-xl bg-amber-500/10 border border-amber-500/20 p-2.5 text-center">
                  <div className="text-base font-bold text-amber-700 dark:text-amber-300">{report.repairable_count}</div>
                  <div className="text-[11px] text-amber-700/80 dark:text-amber-300/80">可自动修复</div>
                </div>
                <div className="rounded-xl bg-slate-500/10 border border-slate-500/20 p-2.5 text-center">
                  <div className="text-base font-bold text-slate-700 dark:text-slate-200">{report.manual_count + report.unmounted_count}</div>
                  <div className="text-[11px] text-slate-500 dark:text-slate-400">需人工/离线</div>
                </div>
              </div>

              {report.items.length === 0 ? (
                <div className="rounded-xl bg-emerald-500/10 border border-emerald-500/25 p-4 flex items-center gap-3 text-emerald-800 dark:text-emerald-200">
                  <CheckCircle2 className="w-5 h-5 shrink-0 text-emerald-600 dark:text-emerald-400" />
                  <div>
                    <div className="font-semibold">全部目录与保护区状态完全健康</div>
                    <div className="text-[11px] opacity-80 mt-0.5">未发现入口丢失、权限异常或孤立保护区目录。</div>
                  </div>
                </div>
              ) : (
                <div className="space-y-2">
                  {report.items.map((item, idx) => (
                    <div
                      key={`${item.entry_id}-${item.kind}-${idx}`}
                      className="rounded-xl bg-white/80 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-3 space-y-1"
                    >
                      <div className="flex items-center justify-between gap-2">
                        <span className="font-mono text-[11.5px] font-semibold text-slate-800 dark:text-slate-100 truncate">
                          {item.display_path}
                        </span>
                        <span
                          className={`px-2 py-0.5 rounded-full text-[10px] font-semibold shrink-0 ${
                            item.auto_repairable
                              ? 'bg-amber-500/15 text-amber-700 dark:text-amber-300'
                              : 'bg-slate-500/15 text-slate-600 dark:text-slate-300'
                          }`}
                        >
                          {item.auto_repairable ? '可一键修复' : '人工核对'}
                        </span>
                      </div>
                      <div className="text-slate-700 dark:text-slate-200">{item.summary}</div>
                      <div className="text-[11px] text-slate-500 dark:text-slate-400">建议：{item.repair_hint}</div>
                    </div>
                  ))}
                </div>
              )}

              {hasDeadRecords && (
                <label className="flex items-center gap-2 text-xs text-slate-700 dark:text-slate-300 cursor-pointer select-none pt-1">
                  <input
                    type="checkbox"
                    checked={cleanDead}
                    onChange={(e) => setCleanDead(e.target.checked)}
                    className="w-3.5 h-3.5 rounded accent-[#0067C0]"
                  />
                  <span>同时清理已失联的目录记录（仅移除列表项，绝不删除任何磁盘文件）</span>
                </label>
              )}

              {repairLog.length > 0 && (
                <div className="rounded-xl bg-emerald-500/10 border border-emerald-500/20 p-3 space-y-1 text-[11px] text-emerald-800 dark:text-emerald-200">
                  <div className="font-semibold">最近一次修复记录：</div>
                  {repairLog.map((line, i) => (
                    <div key={i}>• {line}</div>
                  ))}
                </div>
              )}
            </>
          )}

          {err && (
            <div className="px-3 py-2 rounded-lg text-xs bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
              {err}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex items-center justify-between gap-2">
          {report ? (
            <button
              type="button"
              onClick={() => runScan(password)}
              disabled={loading}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
            >
              <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
              重新扫描
            </button>
          ) : (
            <span />
          )}
          <div className="flex items-center gap-2">
            <button type="button" onClick={onClose} className="px-3.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle">
              关闭
            </button>
            {report && canRepair && (
              <button
                type="button"
                onClick={handleRepair}
                disabled={loading}
                className="inline-flex items-center gap-1.5 px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
              >
                <Wrench className="w-3.5 h-3.5" />
                {loading ? '修复中…' : '一键安全修复'}
              </button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

/* ==================== 3. Emergency Recovery Modal ==================== */
export const RecoveryModal: React.FC<{
  open: boolean;
  sysInfo: SystemInfo | null;
  onClose: () => void;
  onNotify: (msg: string, tone?: NotifyTone) => void;
}> = ({ open, sysInfo, onClose, onNotify }) => {
  if (!open) return null;
  const isWin = sysInfo?.os !== 'linux';

  const winCmd = `:: 以管理员身份打开 CMD，将 <ID> 替换为保护区内的 32 位目录名
:: 1. 查看状态文件记录的 vault_path 与原路径
type "%LOCALAPPDATA%\\DirLocker\\state.json"

:: 2. 解除保护区封锁权限（以 D 盘为例，C 盘保护区位于 %LOCALAPPDATA%\\DirLocker\\Vault）
icacls "D:\\$目录密匣_重要数据保护区_切勿删除" /remove:d *S-1-1-0
icacls "D:\\$目录密匣_重要数据保护区_切勿删除\\<ID>" /remove:d *S-1-1-0
attrib -h -s "D:\\$目录密匣_重要数据保护区_切勿删除\\<ID>"

:: 3. 移回原路径（数据完好无损）
move "D:\\$目录密匣_重要数据保护区_切勿删除\\<ID>" "D:\\你的原目录名"`;

  const linuxCmd = `# 1. 查看状态文件，确认原路径 (path) 与保护区路径 (vault_path)
sudo cat /var/lib/dirlocker/uid_$(id -u).json

# 2. 解除保护区目录的防删位并移回原路径
sudo chattr -i "<vault_path>"
sudo mv "<vault_path>" "<原路径>"
sudo chown -R $(id -u):$(id -g) "<原路径>"
sudo chmod 755 "<原路径>"

# 3. 删除原位残留的 .desktop 锁标文件
sudo chattr -i "<原路径>.desktop" 2>/dev/null || true
sudo rm -f "<原路径>.desktop"`;

  const code = isWin ? winCmd : linuxCmd;

  const handleCopy = () => {
    navigator.clipboard
      ?.writeText(code)
      .then(() => onNotify('已复制应急恢复命令', 'success'))
      .catch(() => {});
  };

  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[600px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-emerald-500/12 text-emerald-600 dark:text-emerald-400 flex items-center justify-center">
              <LifeBuoy className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">应急数据恢复说明</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                本软件绝不加密任何文件内容，即使遗忘密码或卸载程序也可 100% 完整找回
              </p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="px-5 py-4 space-y-3 text-xs">
          <div className="rounded-xl bg-emerald-500/10 border border-emerald-500/20 p-3 flex items-start gap-2.5 text-emerald-900 dark:text-emerald-200 leading-relaxed">
            <ShieldCheck className="w-4 h-4 text-emerald-600 dark:text-emerald-400 shrink-0 mt-0.5" />
            <div>
              <strong>零数据丢失保障：</strong>您的所有文件均原样存放在同分区的系统保护区内，未经过任何加密或改写。状态文件路径：
              <code className="font-mono ml-1 text-[11px]">{sysInfo?.statePath}</code>
            </div>
          </div>

          <div className="relative">
            <button
              type="button"
              onClick={handleCopy}
              className="absolute top-2.5 right-2.5 inline-flex items-center gap-1 px-2.5 py-1 rounded-md text-[11px] font-medium bg-white/90 dark:bg-slate-800/90 border border-slate-200 dark:border-white/15 text-slate-700 dark:text-slate-200 hover:text-[#0067C0] dark:hover:text-[#60CDFF] transition-colors"
            >
              <Copy className="w-3 h-3" />
              复制命令
            </button>
            <pre className="rounded-xl bg-slate-900 text-slate-100 p-4 pr-24 text-[11.5px] font-mono leading-relaxed overflow-x-auto select-text">
              {code}
            </pre>
          </div>
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex justify-end">
          <button type="button" onClick={onClose} className="px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary">
            我知道了
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 4. Quit Confirmation Modal ==================== */
export const QuitConfirmModal: React.FC<{
  open: boolean;
  unlockedCount: number;
  onClose: () => void;
  onRelockAndQuit: () => void;
  onDirectQuit: () => void;
}> = ({ open, unlockedCount, onClose, onRelockAndQuit, onDirectQuit }) => {
  if (!open) return null;
  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[430px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-amber-500/15 text-amber-600 dark:text-amber-400 flex items-center justify-center">
              <AlertTriangle className="w-4 h-4" />
            </div>
            <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">退出 目录密匣</h3>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="px-5 py-4 text-xs text-slate-600 dark:text-slate-300 leading-relaxed">
          当前仍有 <strong className="text-amber-600 dark:text-amber-400">{unlockedCount}</strong> 个目录处于「已解锁」状态。退出程序后，全局快捷键与闲置自动回锁将暂停。是否在退出前一键回锁所有目录？
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex items-center justify-end gap-2">
          <button type="button" onClick={onClose} className="px-3 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle">
            取消
          </button>
          <button
            type="button"
            onClick={onDirectQuit}
            className="inline-flex items-center gap-1 px-3 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
          >
            <Power className="w-3.5 h-3.5" />
            直接退出
          </button>
          <button
            type="button"
            onClick={onRelockAndQuit}
            className="inline-flex items-center gap-1.5 px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            <Lock className="w-3.5 h-3.5" />
            回锁并退出
          </button>
        </div>
      </div>
    </div>
  );
};
