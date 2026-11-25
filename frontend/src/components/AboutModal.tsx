import React from 'react';
import {
  CheckCircle2,
  Copy,
  ExternalLink,
  Monitor,
  Scale,
  ShieldCheck,
  Sparkles,
  Users,
  X,
} from 'lucide-react';
import type { NotifyTone, SystemInfo } from '../types';
import { AppLogo } from './FluentIcons';
import { backend } from '../lib/wails';

interface AboutModalProps {
  open: boolean;
  sysInfo: SystemInfo | null;
  hotkey: string;
  onClose: () => void;
  onNotify: (msg: string, tone?: NotifyTone) => void;
}

const REPO_URL = 'https://github.com/yinchaow/DirLocker';

export const AboutModal: React.FC<AboutModalProps> = ({
  open,
  sysInfo,
  hotkey,
  onClose,
  onNotify,
}) => {
  if (!open) return null;

  const archLabel = sysInfo?.arch === 'arm64' ? 'arm64' : 'x64';
  const platformLabel =
    sysInfo?.os === 'windows'
      ? `Windows 10 / 11 (${archLabel})`
      : sysInfo?.os === 'linux'
      ? `银河麒麟 V10 SP1 / Ubuntu (${archLabel})`
      : 'Windows 10 / 11 (x64)';

  const handleCopyURL = () => {
    navigator.clipboard
      ?.writeText(REPO_URL)
      .then(() => onNotify('已复制项目主页链接', 'info'))
      .catch(() => {});
  };

  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-900/35 dark:bg-black/60 backdrop-blur-[6px] animate-fade-in"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-[540px] rounded-2xl acrylic-surface overflow-hidden animate-modal-in"
      >
        {/* Top Hero */}
        <div className="px-6 pt-6 pb-5 flex items-start justify-between gap-4 border-b border-slate-200/70 dark:border-white/10">
          <div className="flex items-center gap-4">
            <AppLogo className="w-14 h-14 shrink-0 drop-shadow-md" />
            <div>
              <div className="flex items-center gap-2.5">
                <h2 className="text-xl font-bold tracking-tight text-slate-900 dark:text-white">
                  目录密匣
                </h2>
                <span className="px-2 py-0.5 rounded-full text-[11px] font-semibold bg-[#0067C0]/12 text-[#0067C0] dark:bg-[#60CDFF]/15 dark:text-[#60CDFF]">
                  v{sysInfo?.appVersion || '3.0.0'}
                </span>
              </div>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-1">
                DirLocker · 跨平台轻量级目录密码保护工具
              </p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 hover:bg-black/5 dark:hover:bg-white/10 transition-colors"
            title="关闭"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        {/* Metadata Rows */}
        <div className="px-6 py-4 space-y-3 text-xs">
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] divide-y divide-slate-200/60 dark:divide-white/[0.06]">
            <div className="px-4 py-2.5 flex items-center justify-between gap-3">
              <span className="inline-flex items-center gap-2 text-slate-500 dark:text-slate-400 shrink-0">
                <Monitor className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                适用平台
              </span>
              <span className="text-right font-medium text-slate-800 dark:text-slate-100">
                {platformLabel}
              </span>
            </div>

            <div className="px-4 py-2.5 flex items-center justify-between gap-3">
              <span className="inline-flex items-center gap-2 text-slate-500 dark:text-slate-400 shrink-0">
                <Users className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                开发人员
              </span>
              <span className="font-semibold text-slate-800 dark:text-slate-100">
                yinchaow &amp; Gemini
              </span>
            </div>

            <div className="px-4 py-2.5 flex items-center justify-between gap-3">
              <span className="inline-flex items-center gap-2 text-slate-500 dark:text-slate-400 shrink-0">
                <Scale className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                开源协议
              </span>
              <span className="font-mono font-medium text-slate-800 dark:text-slate-100">
                MIT License
              </span>
            </div>

            <div className="px-4 py-2.5 flex items-center justify-between gap-3">
              <span className="inline-flex items-center gap-2 text-slate-500 dark:text-slate-400 shrink-0">
                <ExternalLink className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
                项目主页
              </span>
              <div className="flex items-center gap-1.5 min-w-0">
                <button
                  type="button"
                  onClick={() => backend.openURL(REPO_URL)}
                  className="font-mono text-[#0067C0] dark:text-[#60CDFF] hover:underline truncate"
                >
                  {REPO_URL}
                </button>
                <button
                  type="button"
                  onClick={handleCopyURL}
                  className="p-1 rounded hover:bg-black/5 dark:hover:bg-white/10 text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 shrink-0"
                  title="复制主页地址"
                >
                  <Copy className="w-3 h-3" />
                </button>
              </div>
            </div>
          </div>

          {/* Core Features */}
          <div className="rounded-xl bg-white/75 dark:bg-white/[0.04] border border-slate-200/70 dark:border-white/[0.08] p-4 space-y-2">
            <div className="flex items-center gap-1.5 font-semibold text-slate-800 dark:text-slate-100">
              <Sparkles className="w-3.5 h-3.5 text-[#0067C0] dark:text-[#60CDFF]" />
              核心功能
            </div>
            <ul className="space-y-1.5 text-[11.5px] text-slate-600 dark:text-slate-300 leading-relaxed">
              <li className="flex items-start gap-2">
                <CheckCircle2 className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400 shrink-0 mt-0.5" />
                <span>
                  <strong>毫秒级瞬时加解锁</strong>：基于同盘保护区与系统级权限封锁，绝不修改、不加密目录内任何原始文件内容。
                </span>
              </li>
              <li className="flex items-start gap-2">
                <CheckCircle2 className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400 shrink-0 mt-0.5" />
                <span>
                  <strong>灵活双层密码体系</strong>：支持全局主密码统一管理或单目录独立密码，PBKDF2-HMAC-SHA256 防暴力退避校验。
                </span>
              </li>
              <li className="flex items-start gap-2">
                <CheckCircle2 className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400 shrink-0 mt-0.5" />
                <span>
                  <strong>快捷回锁与防占用保护</strong>：支持全局快捷键（当前{' '}
                  <code className="font-mono text-[#0067C0] dark:text-[#60CDFF]">{hotkey}</code>
                  ）、系统闲置自动回锁，以及正在编辑文档时的占用检测。
                </span>
              </li>
              <li className="flex items-start gap-2">
                <CheckCircle2 className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400 shrink-0 mt-0.5" />
                <span>
                  <strong>零丢失底线与一键体检</strong>：内置状态体检与安全修复、应急命令行恢复指引，断电/迁移均不丢失用户数据。
                </span>
              </li>
            </ul>
          </div>
        </div>

        {/* Footer */}
        <div className="px-6 py-3.5 bg-slate-100/60 dark:bg-white/[0.02] border-t border-slate-200/70 dark:border-white/10 flex items-center justify-between text-[11px] text-slate-500 dark:text-slate-400">
          <span className="inline-flex items-center gap-1.5">
            <ShieldCheck className="w-3.5 h-3.5 text-emerald-600 dark:text-emerald-400" />
            纯本地离线运行 · 零数据上传 · Copyright © 2026 yinchaow
          </span>
          <button
            type="button"
            onClick={onClose}
            className="px-4 py-1.5 rounded-lg text-xs font-medium fluent-btn-primary"
          >
            确定
          </button>
        </div>
      </div>
    </div>
  );
};
