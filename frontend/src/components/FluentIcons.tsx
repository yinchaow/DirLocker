import React, { useState } from 'react';
import { Eye, EyeOff } from 'lucide-react';

/* Unified Fluent Design 2 App Logo — sibling of DocPurger & matches app.ico / dirlocker.png */
export const AppLogo: React.FC<{ className?: string }> = ({ className = 'w-6 h-6' }) => (
  <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg" className={className}>
    <defs>
      <linearGradient id="dlAppBg" x1="6" y1="4" x2="58" y2="60" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#36B4F8" />
        <stop offset="52%" stopColor="#0078D4" />
        <stop offset="100%" stopColor="#004E98" />
      </linearGradient>
      <linearGradient id="dlFolderBody" x1="14" y1="20" x2="46" y2="50" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#FFFFFF" stopOpacity="0.98" />
        <stop offset="100%" stopColor="#DCEEFF" stopOpacity="0.9" />
      </linearGradient>
      <linearGradient id="dlBadge" x1="34" y1="30" x2="58" y2="56" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#1B5EA8" />
        <stop offset="100%" stopColor="#0C2E59" />
      </linearGradient>
    </defs>
    {/* Squircle backdrop */}
    <rect x="3" y="3" width="58" height="58" rx="15" fill="url(#dlAppBg)" />
    <rect x="3.5" y="3.5" width="57" height="57" rx="14.5" stroke="white" strokeOpacity="0.28" />
    {/* Sparkle accent top-right */}
    <path
      d="M50 11L51.2 14.3L54.5 15.5L51.2 16.7L50 20L48.8 16.7L45.5 15.5L48.8 14.3L50 11Z"
      fill="white"
      fillOpacity="0.92"
    />
    {/* Frosted pearl-white folder */}
    <path
      d="M14 21C14 18.79 15.79 17 18 17H27.4C28.5 17 29.55 17.46 30.3 18.27L32.7 20.85H45C47.21 20.85 49 22.64 49 24.85V44C49 46.21 47.21 48 45 48H18C15.79 48 14 46.21 14 44V21Z"
      fill="url(#dlFolderBody)"
    />
    {/* Folder subtle fold highlight */}
    <path
      d="M14 26H49V44C49 46.21 47.21 48 45 48H18C15.79 48 14 46.21 14 44V26Z"
      fill="white"
      fillOpacity="0.35"
    />
    {/* Ocean-acrylic padlock badge */}
    <circle cx="44" cy="43" r="12.5" fill="url(#dlBadge)" stroke="white" strokeWidth="2.2" />
    <path
      d="M40.3 41.2V38.8C40.3 36.75 41.96 35.1 44 35.1C46.04 35.1 47.7 36.75 47.7 38.8V41.2"
      stroke="white"
      strokeWidth="2.2"
      strokeLinecap="round"
    />
    <rect x="38.2" y="41" width="11.6" height="8.6" rx="2.4" fill="white" />
    <circle cx="44" cy="45.2" r="1.35" fill="#0F3C73" />
  </svg>
);

/* Unified Fluent Design 2 Locked Folder Icon — matches folder_lock.ico & dirlocker-folder.png */
export const LockedFolderIcon: React.FC<{ className?: string }> = ({ className = 'w-9 h-9' }) => (
  <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg" className={className}>
    <defs>
      <linearGradient id="dlLfBack" x1="8" y1="12" x2="54" y2="52" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#1B8CE2" />
        <stop offset="100%" stopColor="#0052A3" />
      </linearGradient>
      <linearGradient id="dlLfFront" x1="8" y1="22" x2="54" y2="54" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#48C2FF" />
        <stop offset="55%" stopColor="#0D84E0" />
        <stop offset="100%" stopColor="#005FB8" />
      </linearGradient>
      <linearGradient id="dlLfBadge" x1="34" y1="30" x2="58" y2="56" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#1A5AA3" />
        <stop offset="100%" stopColor="#0A2952" />
      </linearGradient>
    </defs>
    {/* Folder back */}
    <path
      d="M8 16C8 13.24 10.24 11 13 11H24.6C26 11 27.33 11.6 28.27 12.64L31.4 16H51C53.76 16 56 18.24 56 21V48C56 50.76 53.76 53 51 53H13C10.24 53 8 50.76 8 48V16Z"
      fill="url(#dlLfBack)"
    />
    {/* Crisp white document peek */}
    <rect x="14" y="18.5" width="36" height="12" rx="2.5" fill="white" fillOpacity="0.93" />
    {/* Folder front */}
    <path
      d="M8 25C8 22.79 9.79 21 12 21H52C54.21 21 56 22.79 56 25V48C56 50.76 53.76 53 51 53H13C10.24 53 8 50.76 8 48V25Z"
      fill="url(#dlLfFront)"
    />
    <path d="M12 21.6H52" stroke="white" strokeOpacity="0.4" strokeWidth="1.2" strokeLinecap="round" />
    {/* Padlock badge */}
    <circle cx="46" cy="44" r="12.5" fill="url(#dlLfBadge)" stroke="white" strokeWidth="2.2" />
    <path
      d="M42.3 42.2V39.8C42.3 37.75 43.96 36.1 46 36.1C48.04 36.1 49.7 37.75 49.7 39.8V42.2"
      stroke="white"
      strokeWidth="2.2"
      strokeLinecap="round"
    />
    <rect x="40.2" y="42" width="11.6" height="8.6" rx="2.4" fill="white" />
    <circle cx="46" cy="46.2" r="1.35" fill="#0D386E" />
  </svg>
);

/* Fluent Design 2 Unlocked Folder Icon */
export const UnlockedFolderIcon: React.FC<{ className?: string }> = ({ className = 'w-9 h-9' }) => (
  <svg viewBox="0 0 64 64" fill="none" xmlns="http://www.w3.org/2000/svg" className={className}>
    <defs>
      <linearGradient id="dlUfBack" x1="8" y1="12" x2="54" y2="52" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#E6A12E" />
        <stop offset="100%" stopColor="#C47C12" />
      </linearGradient>
      <linearGradient id="dlUfFront" x1="8" y1="22" x2="54" y2="54" gradientUnits="userSpaceOnUse">
        <stop offset="0%" stopColor="#FFD86B" />
        <stop offset="55%" stopColor="#F5BA3B" />
        <stop offset="100%" stopColor="#E19B1B" />
      </linearGradient>
    </defs>
    <path
      d="M8 16C8 13.24 10.24 11 13 11H24.6C26 11 27.33 11.6 28.27 12.64L31.4 16H51C53.76 16 56 18.24 56 21V48C56 50.76 53.76 53 51 53H13C10.24 53 8 50.76 8 48V16Z"
      fill="url(#dlUfBack)"
    />
    <rect x="14" y="18.5" width="36" height="12" rx="2.5" fill="white" fillOpacity="0.92" />
    <path
      d="M8 25C8 22.79 9.79 21 12 21H52C54.21 21 56 22.79 56 25V48C56 50.76 53.76 53 51 53H13C10.24 53 8 50.76 8 48V25Z"
      fill="url(#dlUfFront)"
    />
    <path d="M12 21.6H52" stroke="white" strokeOpacity="0.45" strokeWidth="1.2" strokeLinecap="round" />
    <circle cx="46" cy="44" r="11.5" fill="#10B981" stroke="white" strokeWidth="2.2" />
    <path
      d="M41.5 44.3L44.6 47.4L50.8 41.2"
      stroke="white"
      strokeWidth="2.4"
      strokeLinecap="round"
      strokeLinejoin="round"
    />
  </svg>
);

/* Windows 11 Fluent Toggle Switch (identical to DocPurger) */
export const FluentToggle: React.FC<{
  checked: boolean;
  onChange: (next: boolean) => void;
  disabled?: boolean;
}> = ({ checked, onChange, disabled }) => (
  <button
    type="button"
    role="switch"
    aria-checked={checked}
    disabled={disabled}
    onClick={() => !disabled && onChange(!checked)}
    className={`relative inline-flex h-[20px] w-[38px] shrink-0 items-center rounded-full border transition-all ${
      checked
        ? 'bg-[#0067C0] border-[#0067C0] dark:bg-[#60CDFF] dark:border-[#60CDFF]'
        : 'bg-slate-200/80 border-slate-300/90 dark:bg-white/10 dark:border-white/20'
    } ${disabled ? 'opacity-50 cursor-not-allowed' : ''}`}
  >
    <span
      className={`inline-block h-[13px] w-[13px] rounded-full shadow-sm transition-transform ${
        checked
          ? 'translate-x-[20px] bg-white dark:bg-[#0A1929]'
          : 'translate-x-[3px] bg-slate-500 dark:bg-slate-300'
      }`}
    />
  </button>
);

/* Fluent Password Input with Eye Toggle */
export const PasswordInput: React.FC<{
  value: string;
  onChange: (v: string) => void;
  placeholder?: string;
  autoFocus?: boolean;
  disabled?: boolean;
  onEnter?: () => void;
  className?: string;
}> = ({ value, onChange, placeholder, autoFocus, disabled, onEnter, className = '' }) => {
  const [visible, setVisible] = useState(false);
  return (
    <div className={`relative ${className}`}>
      <input
        type={visible ? 'text' : 'password'}
        value={value}
        autoFocus={autoFocus}
        disabled={disabled}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && onEnter) {
            e.preventDefault();
            onEnter();
          }
        }}
        placeholder={placeholder}
        className="fluent-input w-full rounded-lg pl-3 pr-9 py-2 text-[13px] text-slate-900 dark:text-slate-100 placeholder:text-slate-400"
      />
      <button
        type="button"
        tabIndex={-1}
        onClick={() => setVisible((v) => !v)}
        className="absolute right-2 top-1/2 -translate-y-1/2 p-1 rounded text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 transition-colors"
        title={visible ? '隐藏密码' : '显示密码'}
      >
        {visible ? <EyeOff className="w-3.5 h-3.5" /> : <Eye className="w-3.5 h-3.5" />}
      </button>
    </div>
  );
};
