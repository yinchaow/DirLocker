import React, { useEffect, useState } from 'react';
import { AlertTriangle, FolderLock, KeyRound, Lock, ShieldAlert, Trash2, Unlock, X } from 'lucide-react';
import type { DirEntry } from '../types';
import { PasswordInput } from './FluentIcons';

const DISCLAIMER_TEXT =
  '本软件为纯本地离线工具，不联网、不上传密码、不提供“找回密码”功能。请务必妥善牢记您设置的密码；若遗忘密码，您仍可通过管理员权限按《应急恢复说明》找回目录数据，但软件本身无法直接重置或找回密码。';

const DisclaimerBox: React.FC<{
  checked: boolean;
  onChange: (v: boolean) => void;
}> = ({ checked, onChange }) => (
  <div className="rounded-xl bg-amber-50/85 dark:bg-amber-500/10 border border-amber-300/70 dark:border-amber-400/25 p-3 space-y-2">
    <div className="flex items-start gap-2 text-[11.5px] leading-relaxed text-amber-900 dark:text-amber-200">
      <AlertTriangle className="w-4 h-4 text-amber-600 dark:text-amber-400 shrink-0 mt-0.5" />
      <span>{DISCLAIMER_TEXT}</span>
    </div>
    <label className="flex items-center gap-2 text-[12px] font-medium text-amber-950 dark:text-amber-100 cursor-pointer select-none pt-0.5">
      <input
        type="checkbox"
        checked={checked}
        onChange={(e) => onChange(e.target.checked)}
        className="w-3.5 h-3.5 rounded accent-[#0067C0]"
      />
      <span>我已阅读并知晓以上风险提示，会妥善保管密码</span>
    </label>
  </div>
);

/* ==================== 1. Set / Change Master Password Modal ==================== */
export const MasterPasswordModal: React.FC<{
  open: boolean;
  isInitial: boolean;
  onClose: () => void;
  onSubmit: (oldPw: string, newPw: string) => Promise<void>;
}> = ({ open, isInitial, onClose, onSubmit }) => {
  const [oldPw, setOldPw] = useState('');
  const [newPw, setNewPw] = useState('');
  const [confirmPw, setConfirmPw] = useState('');
  const [ack, setAck] = useState(false);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setOldPw('');
      setNewPw('');
      setConfirmPw('');
      setAck(false);
      setErr('');
      setBusy(false);
    }
  }, [open]);

  if (!open) return null;

  const handleSubmit = async () => {
    setErr('');
    if (!isInitial && !oldPw) {
      setErr('请输入当前主密码');
      return;
    }
    if (newPw.length < 4) {
      setErr('新主密码长度至少需要 4 个字符');
      return;
    }
    if (newPw !== confirmPw) {
      setErr('两次输入的新主密码不一致');
      return;
    }
    if (!ack) {
      setErr('请先勾选确认知晓密码保管风险提示');
      return;
    }
    setBusy(true);
    try {
      await onSubmit(oldPw, newPw);
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '保存失败'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div
      onClick={isInitial ? undefined : onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[460px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center">
              <KeyRound className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">
                {isInitial ? '初始化全局主密码' : '修改全局主密码'}
              </h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                {isInitial ? '首次使用请先设置全局主密码，用于解锁目录与保护设置' : '验证原主密码后即可更新全局主密码'}
              </p>
            </div>
          </div>
          {!isInitial && (
            <button
              type="button"
              onClick={onClose}
              className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10"
            >
              <X className="w-4 h-4" />
            </button>
          )}
        </div>

        <div className="px-5 py-4 space-y-3">
          {!isInitial && (
            <div className="space-y-1">
              <label className="text-xs font-medium text-slate-700 dark:text-slate-300">当前主密码</label>
              <PasswordInput
                value={oldPw}
                onChange={setOldPw}
                placeholder="请输入原主密码"
                autoFocus
                disabled={busy}
              />
            </div>
          )}

          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">
              {isInitial ? '设置主密码' : '新主密码'}（至少 4 位）
            </label>
            <PasswordInput
              value={newPw}
              onChange={setNewPw}
              placeholder="输入新主密码"
              autoFocus={isInitial}
              disabled={busy}
            />
          </div>

          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">确认主密码</label>
            <PasswordInput
              value={confirmPw}
              onChange={setConfirmPw}
              placeholder="再次输入以确认"
              disabled={busy}
              onEnter={handleSubmit}
            />
          </div>

          <DisclaimerBox checked={ack} onChange={setAck} />

          {err && (
            <div className="px-3 py-2 rounded-lg text-xs bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
              {err}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex justify-end gap-2">
          {!isInitial && (
            <button
              type="button"
              onClick={onClose}
              disabled={busy}
              className="px-3.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
            >
              取消
            </button>
          )}
          <button
            type="button"
            onClick={handleSubmit}
            disabled={busy || !ack}
            className="px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            {busy ? '保存中…' : isInitial ? '保存主密码并开始使用' : '确认修改'}
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 2. Add & Lock Directory Modal ==================== */
export const LockDirModal: React.FC<{
  open: boolean;
  paths: string[];
  onClose: () => void;
  onSubmit: (paths: string[], useCustom: boolean, customPw: string) => Promise<void>;
}> = ({ open, paths, onClose, onSubmit }) => {
  const [useCustom, setUseCustom] = useState(false);
  const [customPw, setCustomPw] = useState('');
  const [confirmPw, setConfirmPw] = useState('');
  const [ack, setAck] = useState(false);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setUseCustom(false);
      setCustomPw('');
      setConfirmPw('');
      setAck(false);
      setErr('');
      setBusy(false);
    }
  }, [open, paths]);

  if (!open || paths.length === 0) return null;

  const handleSubmit = async () => {
    setErr('');
    if (useCustom) {
      if (customPw.length < 4) {
        setErr('独立密码长度至少需要 4 个字符');
        return;
      }
      if (customPw !== confirmPw) {
        setErr('两次输入的独立密码不一致');
        return;
      }
      if (!ack) {
        setErr('设置独立密码时，请先勾选确认知晓密码保管风险提示');
        return;
      }
    }
    setBusy(true);
    try {
      await onSubmit(paths, useCustom, useCustom ? customPw : '');
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '锁定失败'));
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
        className="w-full max-w-[470px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center">
              <FolderLock className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">
                {paths.length > 1 ? `批量添加并锁定 ${paths.length} 个目录` : '添加并锁定目录'}
              </h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                仅封锁目录访问入口，绝不修改或加密内部文件
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

        <div className="px-5 py-4 space-y-3.5">
          {/* Target Paths Preview */}
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-2.5 max-h-28 overflow-y-auto space-y-1">
            {paths.map((p) => (
              <div key={p} className="text-xs font-mono text-slate-700 dark:text-slate-200 truncate" title={p}>
                {p}
              </div>
            ))}
          </div>

          {/* Password Mode Selector */}
          <div className="space-y-2">
            <div className="text-xs font-medium text-slate-700 dark:text-slate-300">选择解锁密码方式</div>
            <div className="grid grid-cols-2 gap-2">
              <button
                type="button"
                onClick={() => setUseCustom(false)}
                className={`p-3 rounded-xl border text-left transition-all ${
                  !useCustom
                    ? 'bg-[#0067C0]/10 border-[#0067C0] dark:bg-[#60CDFF]/15 dark:border-[#60CDFF]'
                    : 'bg-white/60 dark:bg-white/[0.03] border-slate-200/80 dark:border-white/10 hover:border-slate-300'
                }`}
              >
                <div className="text-xs font-semibold text-slate-900 dark:text-white">全局主密码（推荐）</div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  统一使用主密码解锁，无需额外记忆
                </div>
              </button>
              <button
                type="button"
                onClick={() => setUseCustom(true)}
                className={`p-3 rounded-xl border text-left transition-all ${
                  useCustom
                    ? 'bg-[#0067C0]/10 border-[#0067C0] dark:bg-[#60CDFF]/15 dark:border-[#60CDFF]'
                    : 'bg-white/60 dark:bg-white/[0.03] border-slate-200/80 dark:border-white/10 hover:border-slate-300'
                }`}
              >
                <div className="text-xs font-semibold text-slate-900 dark:text-white">设置独立密码</div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  为此目录单独指定密码，与主密码隔离
                </div>
              </button>
            </div>
          </div>

          {useCustom && (
            <div className="space-y-3 animate-fade-in">
              <div className="space-y-1">
                <label className="text-xs font-medium text-slate-700 dark:text-slate-300">独立密码（至少 4 位）</label>
                <PasswordInput
                  value={customPw}
                  onChange={setCustomPw}
                  placeholder="输入独立密码"
                  autoFocus
                  disabled={busy}
                />
              </div>
              <div className="space-y-1">
                <label className="text-xs font-medium text-slate-700 dark:text-slate-300">确认独立密码</label>
                <PasswordInput
                  value={confirmPw}
                  onChange={setConfirmPw}
                  placeholder="再次输入独立密码"
                  disabled={busy}
                  onEnter={handleSubmit}
                />
              </div>
              <DisclaimerBox checked={ack} onChange={setAck} />
            </div>
          )}

          {err && (
            <div className="px-3 py-2 rounded-lg text-xs bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
              {err}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex justify-end gap-2">
          <button
            type="button"
            onClick={onClose}
            disabled={busy}
            className="px-3.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
          >
            取消
          </button>
          <button
            type="button"
            onClick={handleSubmit}
            disabled={busy || (useCustom && !ack)}
            className="inline-flex items-center gap-1.5 px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            <Lock className="w-3.5 h-3.5" />
            {busy ? '正在锁定…' : '立即锁定'}
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 3. Unlock Directory Modal ==================== */
export const UnlockDirModal: React.FC<{
  open: boolean;
  entry: DirEntry | null;
  fallbackPath?: string;
  onClose: () => void;
  onSubmit: (id: string, path: string, pw: string, openAfter: boolean) => Promise<void>;
}> = ({ open, entry, fallbackPath, onClose, onSubmit }) => {
  const [pw, setPw] = useState('');
  const [openAfter, setOpenAfter] = useState(true);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setPw('');
      setOpenAfter(true);
      setErr('');
      setBusy(false);
    }
  }, [open, entry, fallbackPath]);

  if (!open || (!entry && !fallbackPath)) return null;

  const targetPath = entry?.path || fallbackPath || '';
  const targetID = entry?.id || '';
  const isCustom = Boolean(entry?.use_custom_password);

  const handleSubmit = async () => {
    setErr('');
    if (!pw) {
      setErr('请输入解锁密码');
      return;
    }
    setBusy(true);
    try {
      await onSubmit(targetID, targetPath, pw, openAfter);
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '解锁失败'));
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
        className="w-full max-w-[440px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-emerald-500/12 text-emerald-600 dark:text-emerald-400 flex items-center justify-center">
              <Unlock className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">验证密码并解锁目录</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                {isCustom ? '该目录已设置独立密码，请输入其独立密码' : '请输入全局主密码以解锁访问'}
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

        <div className="px-5 py-4 space-y-3.5">
          <div
            className="px-3 py-2 rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] text-xs font-mono text-slate-700 dark:text-slate-200 break-all"
          >
            {targetPath}
          </div>

          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">
              {isCustom ? '目录独立密码' : '全局主密码'}
            </label>
            <PasswordInput
              value={pw}
              onChange={setPw}
              placeholder="输入密码后按回车解锁"
              autoFocus
              disabled={busy}
              onEnter={handleSubmit}
            />
          </div>

          <label className="flex items-center gap-2 text-xs text-slate-700 dark:text-slate-300 cursor-pointer select-none">
            <input
              type="checkbox"
              checked={openAfter}
              onChange={(e) => setOpenAfter(e.target.checked)}
              className="w-3.5 h-3.5 rounded accent-[#0067C0]"
            />
            <span>解锁后立即打开该目录</span>
          </label>

          {err && (
            <div className="px-3 py-2 rounded-lg text-xs bg-rose-500/10 border border-rose-500/25 text-rose-600 dark:text-rose-300 whitespace-pre-line">
              {err}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex justify-end gap-2">
          <button
            type="button"
            onClick={onClose}
            disabled={busy}
            className="px-3.5 py-1.5 rounded-lg text-xs font-medium fluent-btn-subtle"
          >
            取消
          </button>
          <button
            type="button"
            onClick={handleSubmit}
            disabled={busy}
            className="inline-flex items-center gap-1.5 px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            <Unlock className="w-3.5 h-3.5" />
            {busy ? '验证中…' : '立即解锁'}
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 4. Change Directory Password Mode Modal ==================== */
export const ChangeDirPasswordModal: React.FC<{
  open: boolean;
  entry: DirEntry | null;
  onClose: () => void;
  onSubmit: (id: string, currentPw: string, useCustom: boolean, newCustomPw: string) => Promise<void>;
}> = ({ open, entry, onClose, onSubmit }) => {
  const [currentPw, setCurrentPw] = useState('');
  const [useCustom, setUseCustom] = useState(false);
  const [newPw, setNewPw] = useState('');
  const [confirmPw, setConfirmPw] = useState('');
  const [ack, setAck] = useState(false);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open && entry) {
      setCurrentPw('');
      setUseCustom(!entry.use_custom_password ? true : entry.use_custom_password);
      setNewPw('');
      setConfirmPw('');
      setAck(false);
      setErr('');
      setBusy(false);
    }
  }, [open, entry]);

  if (!open || !entry) return null;

  const handleSubmit = async () => {
    setErr('');
    if (!currentPw) {
      setErr('请输入该目录当前的有效密码进行身份验证');
      return;
    }
    if (useCustom) {
      if (newPw.length < 4) {
        setErr('新独立密码长度至少需要 4 个字符');
        return;
      }
      if (newPw !== confirmPw) {
        setErr('两次输入的新独立密码不一致');
        return;
      }
      if (!ack) {
        setErr('请先勾选确认知晓密码保管风险提示');
        return;
      }
    }
    setBusy(true);
    try {
      await onSubmit(entry.id, currentPw, useCustom, useCustom ? newPw : '');
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '修改失败'));
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
        className="w-full max-w-[460px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-[#0067C0]/12 dark:bg-[#60CDFF]/15 text-[#0067C0] dark:text-[#60CDFF] flex items-center justify-center">
              <KeyRound className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">设置目录密码方式</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                当前方式：{entry.use_custom_password ? '独立密码' : '全局主密码'}
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

        <div className="px-5 py-4 space-y-3">
          <div className="px-3 py-2 rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] text-xs font-mono text-slate-700 dark:text-slate-200 break-all">
            {entry.path}
          </div>

          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">
              验证当前密码（{entry.use_custom_password ? '当前独立密码' : '全局主密码'}）
            </label>
            <PasswordInput
              value={currentPw}
              onChange={setCurrentPw}
              placeholder="请输入当前有效密码"
              autoFocus
              disabled={busy}
            />
          </div>

          <div className="grid grid-cols-2 gap-2 pt-1">
            <button
              type="button"
              onClick={() => setUseCustom(false)}
              className={`px-3 py-2.5 rounded-xl border text-left transition-all ${
                !useCustom
                  ? 'bg-[#0067C0]/10 border-[#0067C0] dark:bg-[#60CDFF]/15 dark:border-[#60CDFF]'
                  : 'bg-white/60 dark:bg-white/[0.03] border-slate-200/80 dark:border-white/10'
              }`}
            >
              <div className="text-xs font-semibold text-slate-900 dark:text-white">改用全局主密码</div>
              <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">清除独立密码</div>
            </button>
            <button
              type="button"
              onClick={() => setUseCustom(true)}
              className={`px-3 py-2.5 rounded-xl border text-left transition-all ${
                useCustom
                  ? 'bg-[#0067C0]/10 border-[#0067C0] dark:bg-[#60CDFF]/15 dark:border-[#60CDFF]'
                  : 'bg-white/60 dark:bg-white/[0.03] border-slate-200/80 dark:border-white/10'
              }`}
            >
              <div className="text-xs font-semibold text-slate-900 dark:text-white">使用独立密码</div>
              <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">设置或更换独立密码</div>
            </button>
          </div>

          {useCustom && (
            <div className="space-y-3 animate-fade-in">
              <div className="space-y-1">
                <label className="text-xs font-medium text-slate-700 dark:text-slate-300">新独立密码（至少 4 位）</label>
                <PasswordInput value={newPw} onChange={setNewPw} placeholder="输入新独立密码" disabled={busy} />
              </div>
              <div className="space-y-1">
                <label className="text-xs font-medium text-slate-700 dark:text-slate-300">确认新独立密码</label>
                <PasswordInput
                  value={confirmPw}
                  onChange={setConfirmPw}
                  placeholder="再次输入确认"
                  disabled={busy}
                  onEnter={handleSubmit}
                />
              </div>
              <DisclaimerBox checked={ack} onChange={setAck} />
            </div>
          )}

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
          <button
            type="button"
            onClick={handleSubmit}
            disabled={busy || (useCustom && !ack)}
            className="px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            {busy ? '保存中…' : '确认更新'}
          </button>
        </div>
      </div>
    </div>
  );
};

/* ==================== 5. Remove Directory From Management Modal ==================== */
export const RemoveDirModal: React.FC<{
  open: boolean;
  entry: DirEntry | null;
  onClose: () => void;
  onSubmit: (id: string, pw: string) => Promise<void>;
}> = ({ open, entry, onClose, onSubmit }) => {
  const [pw, setPw] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setPw('');
      setErr('');
      setBusy(false);
    }
  }, [open, entry]);

  if (!open || !entry) return null;

  const handleSubmit = async () => {
    setErr('');
    if (!pw) {
      setErr('请输入密码以确认移出保护列表');
      return;
    }
    setBusy(true);
    try {
      await onSubmit(entry.id, pw);
      onClose();
    } catch (e: any) {
      setErr(String(e?.message || e || '操作失败'));
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
        className="w-full max-w-[440px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        <div className="px-5 py-4 flex items-center justify-between border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-2.5">
            <div className="w-8 h-8 rounded-lg bg-rose-500/12 text-rose-600 dark:text-rose-400 flex items-center justify-center">
              <Trash2 className="w-4 h-4" />
            </div>
            <div>
              <h3 className="text-[14.5px] font-bold text-slate-900 dark:text-white">移出保护列表</h3>
              <p className="text-[11px] text-slate-500 dark:text-slate-400">
                移出前会自动解锁恢复目录，绝不删除目录内任何文件
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

        <div className="px-5 py-4 space-y-3.5">
          <div className="px-3 py-2 rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] text-xs font-mono text-slate-700 dark:text-slate-200 break-all">
            {entry.path}
          </div>
          <div className="space-y-1">
            <label className="text-xs font-medium text-slate-700 dark:text-slate-300">
              验证{entry.use_custom_password ? '目录独立密码' : '全局主密码'}
            </label>
            <PasswordInput
              value={pw}
              onChange={setPw}
              placeholder="输入密码确认移出"
              autoFocus
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
          <button
            type="button"
            onClick={handleSubmit}
            disabled={busy}
            className="inline-flex items-center gap-1.5 px-4 py-1.5 rounded-lg text-xs font-medium bg-rose-600 hover:bg-rose-700 text-white transition-colors"
          >
            <ShieldAlert className="w-3.5 h-3.5" />
            {busy ? '处理中…' : '确认移出'}
          </button>
        </div>
      </div>
    </div>
  );
};
