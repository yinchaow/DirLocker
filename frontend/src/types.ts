export interface Settings {
  auto_relock_minutes: number;
  require_master_on_open: boolean;
  allow_in_place_fallback: boolean;
  hotkey: string;
}

export interface DirEntry {
  id: string;
  path: string;
  locked: boolean;
  use_custom_password: boolean;
  vault_path: string;
  stub_path: string;
  in_place_acl: boolean;
  created_at: number;
  last_unlocked_at: number;
}

export interface StatusResponse {
  has_master_password: boolean;
  settings: Settings;
  entries: DirEntry[];
  daemon_connected: boolean;
  daemon_error?: string;
  hotkey_active: boolean;
  hotkey_warning?: string;
  tray_available: boolean;
}

export interface LockAllResult {
  locked_count: number;
  failed_count: number;
  failed_details?: string[];
}

export interface HealthItem {
  entry_id: string;
  display_path: string;
  vault_path: string;
  kind: string;
  summary: string;
  repair_hint: string;
  auto_repairable: boolean;
  allow_remove_record: boolean;
}

export interface HealthReport {
  total_entries: number;
  ok_count: number;
  repairable_count: number;
  manual_count: number;
  unmounted_count: number;
  orphan_vault_count: number;
  items: HealthItem[];
}

export interface RepairResult {
  repaired_count: number;
  details: string[];
}

export interface SystemInfo {
  os: string;
  arch: string;
  appVersion: string;
  statePath: string;
  homeDir: string;
  trayAvailable: boolean;
  startMode: string;
}

export interface HotkeyTestResult {
  ok: boolean;
  message: string;
}

export interface ActionRequestEvent {
  type: 'unlock' | 'lock' | 'toggle' | 'post-install' | 'quit-confirm';
  entryId?: string;
  path?: string;
  message?: string;
}

export type NotifyTone = 'info' | 'success' | 'error';
