package core

const (
	AppVersion        = "3.0.1"
	AppNameZH         = "目录密匣"
	AppNameEN         = "DirLocker"
	DefaultHotkey     = "Ctrl+Alt+K"
	LegacyHotkey      = "Ctrl+Alt+L"
	StateFormatVer    = 1
	DefaultPBKDF2Iter = 120000
)

type Settings struct {
	AutoRelockMinutes    int    `json:"auto_relock_minutes"`
	RequireMasterOnOpen  bool   `json:"require_master_on_open"`
	AllowInPlaceFallback bool   `json:"allow_in_place_fallback"`
	Hotkey               string `json:"hotkey"`
}

func DefaultSettings() Settings {
	return Settings{
		AutoRelockMinutes:    5,
		RequireMasterOnOpen:  false,
		AllowInPlaceFallback: false,
		Hotkey:               DefaultHotkey,
	}
}

type DirEntry struct {
	ID                string `json:"id"`
	Path              string `json:"path"`
	Locked            bool   `json:"locked"`
	UseCustomPassword bool   `json:"use_custom_password"`
	CustomHash        string `json:"custom_hash,omitempty"`
	OrigMode          uint32 `json:"orig_mode"`
	OrigUID           uint32 `json:"orig_uid"`
	OrigGID           uint32 `json:"orig_gid"`
	SetImmutable      bool   `json:"set_immutable"`
	WinSID            string `json:"win_sid"`
	VaultPath         string `json:"vault_path"`
	StubPath          string `json:"stub_path"`
	InPlaceACL        bool   `json:"in_place_acl"`
	WinOrigAttrs      uint32 `json:"win_orig_attrs"`
	CreatedAt         int64  `json:"created_at"`
	LastUnlockedAt    int64  `json:"last_unlocked_at"`
}

// PublicDirEntry strips password hashes before exposing entries to the frontend.
type PublicDirEntry struct {
	ID                string `json:"id"`
	Path              string `json:"path"`
	Locked            bool   `json:"locked"`
	UseCustomPassword bool   `json:"use_custom_password"`
	VaultPath         string `json:"vault_path"`
	StubPath          string `json:"stub_path"`
	InPlaceACL        bool   `json:"in_place_acl"`
	CreatedAt         int64  `json:"created_at"`
	LastUnlockedAt    int64  `json:"last_unlocked_at"`
}

func (e DirEntry) ToPublic() PublicDirEntry {
	return PublicDirEntry{
		ID:                e.ID,
		Path:              e.Path,
		Locked:            e.Locked,
		UseCustomPassword: e.UseCustomPassword,
		VaultPath:         e.VaultPath,
		StubPath:          e.StubPath,
		InPlaceACL:        e.InPlaceACL,
		CreatedAt:         e.CreatedAt,
		LastUnlockedAt:    e.LastUnlockedAt,
	}
}

type State struct {
	Version    int        `json:"version"`
	MasterHash string     `json:"master_hash"`
	Settings   Settings   `json:"settings"`
	Entries    []DirEntry `json:"entries"`
}

type StatusResponse struct {
	HasMasterPassword bool             `json:"has_master_password"`
	Settings          Settings         `json:"settings"`
	Entries           []PublicDirEntry `json:"entries"`
	DaemonConnected   bool             `json:"daemon_connected"`
	DaemonError       string           `json:"daemon_error,omitempty"`
	HotkeyActive      bool             `json:"hotkey_active"`
	HotkeyWarning     string           `json:"hotkey_warning,omitempty"`
	TrayAvailable     bool             `json:"tray_available"`
}

type QueryResponse struct {
	Managed           bool   `json:"managed"`
	ID                string `json:"id,omitempty"`
	Path              string `json:"path,omitempty"`
	Locked            bool   `json:"locked"`
	UseCustomPassword bool   `json:"use_custom_password"`
	InPlaceACL        bool   `json:"in_place_acl"`
}

type LockAllResult struct {
	LockedCount   int      `json:"locked_count"`
	FailedCount   int      `json:"failed_count"`
	FailedDetails []string `json:"failed_details,omitempty"`
}

type HealthItem struct {
	EntryID           string `json:"entry_id"`
	DisplayPath       string `json:"display_path"`
	VaultPath         string `json:"vault_path"`
	Kind              string `json:"kind"`
	Summary           string `json:"summary"`
	RepairHint        string `json:"repair_hint"`
	AutoRepairable    bool   `json:"auto_repairable"`
	AllowRemoveRecord bool   `json:"allow_remove_record"`
}

type HealthReport struct {
	TotalEntries     int          `json:"total_entries"`
	OKCount          int          `json:"ok_count"`
	RepairableCount  int          `json:"repairable_count"`
	ManualCount      int          `json:"manual_count"`
	UnmountedCount   int          `json:"unmounted_count"`
	OrphanVaultCount int          `json:"orphan_vault_count"`
	Items            []HealthItem `json:"items"`
}

type RepairResult struct {
	RepairedCount int      `json:"repaired_count"`
	Details       []string `json:"details"`
}

type SystemInfo struct {
	OS            string `json:"os"`
	Arch          string `json:"arch"`
	AppVersion    string `json:"appVersion"`
	StatePath     string `json:"statePath"`
	HomeDir       string `json:"homeDir"`
	TrayAvailable bool   `json:"trayAvailable"`
	StartMode     string `json:"startMode"`
}

type ActionRequestEvent struct {
	Type    string `json:"type"` // "unlock", "lock", "toggle", "post-install", "quit-confirm"
	EntryID string `json:"entryId,omitempty"`
	Path    string `json:"path,omitempty"`
	Message string `json:"message,omitempty"`
}
