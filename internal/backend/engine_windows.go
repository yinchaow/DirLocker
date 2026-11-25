//go:build windows

package backend

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"dirlocker/internal/core"
)

type windowsEngine struct {
	mu       sync.Mutex
	store    *core.StateStore
	stateDir string
	throttle core.AuthThrottle
}

func NewEngine() Engine {
	statePath := GetWindowsStatePath()
	eng := &windowsEngine{
		store:    core.NewStateStore(statePath),
		stateDir: filepath.Dir(statePath),
	}
	_ = eng.store.Load()
	return eng
}

func (e *windowsEngine) StatePath() string {
	return e.store.Path
}

func (e *windowsEngine) reloadLocked() error {
	return e.store.Load()
}

func (e *windowsEngine) verifyThrottled(wrongMsg string, check func() bool) error {
	if rem := e.throttle.RemainingCooldownSeconds(); rem > 0 {
		return errors.New(core.FormatCooldownError(rem))
	}
	if check() {
		e.throttle.RecordSuccess()
		return nil
	}
	e.throttle.RecordFailure()
	remNow := e.throttle.RemainingCooldownSeconds()
	if remNow > 0 {
		return errors.New(wrongMsg + "\n" + core.FormatCooldownError(remNow))
	}
	return errors.New(wrongMsg)
}

func (e *windowsEngine) verifyEntryPasswordLocked(entry *core.DirEntry, pw string) bool {
	if entry.UseCustomPassword {
		return core.VerifyPassword(pw, entry.CustomHash)
	}
	return core.VerifyPassword(pw, e.store.State.MasterHash)
}

func (e *windowsEngine) findManagedLocked(id, rawPath string) *core.DirEntry {
	if id != "" {
		if ent := e.store.FindByID(id); ent != nil {
			return ent
		}
	}
	if rawPath == "" {
		return nil
	}
	norm := core.NormalizeWinPath(rawPath)
	if ent := e.store.FindByPath(norm); ent != nil {
		return ent
	}
	lower := strings.ToLower(norm)
	if strings.HasSuffix(lower, ".dlock") {
		if fid, fpath, ok := WinReadDlockFile(norm); ok {
			if fid != "" {
				if ent := e.store.FindByID(fid); ent != nil {
					return ent
				}
			}
			if fpath != "" {
				if ent := e.store.FindByPath(fpath); ent != nil {
					return ent
				}
			}
		}
		trimmed := norm[:len(norm)-len(".dlock")]
		if ent := e.store.FindByPath(trimmed); ent != nil {
			return ent
		}
	}
	for i := range e.store.State.Entries {
		ent := &e.store.State.Entries[i]
		if ent.StubPath != "" && core.PathsEqual(ent.StubPath, norm) {
			return ent
		}
	}
	return nil
}

func (e *windowsEngine) Status() (*core.StatusResponse, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	changed := false
	for i := range e.store.State.Entries {
		if winRepairLockedEntryStub(&e.store.State.Entries[i]) {
			changed = true
		}
	}
	if changed {
		_ = e.store.Save()
	}
	pub := make([]core.PublicDirEntry, 0, len(e.store.State.Entries))
	for _, ent := range e.store.State.Entries {
		pub = append(pub, ent.ToPublic())
	}
	return &core.StatusResponse{
		HasMasterPassword: e.store.HasMasterPassword(),
		Settings:          e.store.State.Settings,
		Entries:           pub,
		DaemonConnected:   true,
	}, nil
}

func (e *windowsEngine) Query(path string) (*core.QueryResponse, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	_ = e.reloadLocked()
	ent := e.findManagedLocked("", path)
	if ent == nil {
		return &core.QueryResponse{Managed: false}, nil
	}
	return &core.QueryResponse{
		Managed:           true,
		ID:                ent.ID,
		Path:              ent.Path,
		Locked:            ent.Locked,
		UseCustomPassword: ent.UseCustomPassword,
		InPlaceACL:        ent.InPlaceACL,
	}, nil
}

func (e *windowsEngine) SetMasterPassword(oldPassword, newPassword string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return err
	}
	if len(newPassword) < 4 {
		return errors.New("主密码长度至少需要 4 个字符")
	}
	if e.store.HasMasterPassword() {
		if err := e.verifyThrottled("原主密码不正确", func() bool {
			return core.VerifyPassword(oldPassword, e.store.State.MasterHash)
		}); err != nil {
			return err
		}
	}
	hash, err := core.HashPassword(newPassword, core.DefaultPBKDF2Iter)
	if err != nil {
		return err
	}
	e.store.State.MasterHash = hash
	return e.store.Save()
}

func (e *windowsEngine) VerifyMaster(password string) (bool, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return false, err
	}
	if !e.store.HasMasterPassword() {
		return false, errors.New("尚未设置主密码")
	}
	if err := e.verifyThrottled("主密码不正确", func() bool {
		return core.VerifyPassword(password, e.store.State.MasterHash)
	}); err != nil {
		return false, err
	}
	return true, nil
}

func (e *windowsEngine) UpdateSettings(password string, settings core.Settings) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return err
	}
	if !e.store.HasMasterPassword() {
		return errors.New("请先设置主密码")
	}
	if err := e.verifyThrottled("主密码错误，未保存设置", func() bool {
		return core.VerifyPassword(password, e.store.State.MasterHash)
	}); err != nil {
		return err
	}
	spec, err := core.ParseHotkey(settings.Hotkey)
	if err != nil {
		return fmt.Errorf("快捷键格式无效: %w", err)
	}
	mins := settings.AutoRelockMinutes
	if mins < 0 {
		mins = 0
	}
	if mins > 1440 {
		mins = 1440
	}
	e.store.State.Settings.AutoRelockMinutes = mins
	e.store.State.Settings.RequireMasterOnOpen = settings.RequireMasterOnOpen
	e.store.State.Settings.AllowInPlaceFallback = settings.AllowInPlaceFallback
	e.store.State.Settings.Hotkey = spec.String()
	return e.store.Save()
}

func (e *windowsEngine) AddAndLock(path string, useCustom bool, customPassword string) (*core.PublicDirEntry, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	if !e.store.HasMasterPassword() {
		return nil, errors.New("请先设置主密码")
	}
	if existing := e.findManagedLocked("", path); existing != nil {
		if existing.Locked {
			pub := existing.ToPublic()
			return &pub, nil
		}
		if err := WinSmartLock(existing, e.stateDir, e.store.State.Settings.AllowInPlaceFallback); err != nil {
			return nil, err
		}
		if err := e.store.Save(); err != nil {
			return nil, err
		}
		pub := existing.ToPublic()
		return &pub, nil
	}

	existingPaths := make([]string, 0, len(e.store.State.Entries))
	for _, ent := range e.store.State.Entries {
		existingPaths = append(existingPaths, ent.Path)
	}
	extraProtected := []string{e.stateDir}
	if self := getSelfExePath(); self != "" {
		extraProtected = append(extraProtected, filepath.Dir(self))
	}
	home, _ := os.UserHomeDir()
	canon, err := core.ValidateLockableDirectory(path, existingPaths, home, extraProtected)
	if err != nil {
		return nil, err
	}

	entry := core.DirEntry{
		ID:                core.GenerateID(),
		Path:              canon,
		UseCustomPassword: useCustom,
		WinSID:            "S-1-1-0",
		CreatedAt:         core.NowEpoch(),
	}
	if useCustom {
		if len(customPassword) < 4 {
			return nil, errors.New("独立密码长度至少需要 4 个字符")
		}
		h, err := core.HashPassword(customPassword, core.DefaultPBKDF2Iter)
		if err != nil {
			return nil, err
		}
		entry.CustomHash = h
	}
	if err := WinSmartLock(&entry, e.stateDir, e.store.State.Settings.AllowInPlaceFallback); err != nil {
		return nil, err
	}
	e.store.State.Entries = append(e.store.State.Entries, entry)
	if err := e.store.Save(); err != nil {
		return nil, err
	}
	pub := entry.ToPublic()
	return &pub, nil
}

func (e *windowsEngine) Lock(id, path string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return err
	}
	entry := e.findManagedLocked(id, path)
	if entry == nil {
		return errors.New("该目录尚未加入目录密匣管理列表")
	}
	if entry.Locked {
		return nil
	}
	if err := WinSmartLock(entry, e.stateDir, e.store.State.Settings.AllowInPlaceFallback); err != nil {
		return err
	}
	return e.store.Save()
}

func (e *windowsEngine) LockAll() (*core.LockAllResult, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	res := &core.LockAllResult{}
	for i := range e.store.State.Entries {
		ent := &e.store.State.Entries[i]
		if ent.Locked {
			continue
		}
		if err := WinSmartLock(ent, e.stateDir, e.store.State.Settings.AllowInPlaceFallback); err != nil {
			res.FailedCount++
			res.FailedDetails = append(res.FailedDetails, filepath.Base(ent.Path)+": "+err.Error())
		} else {
			res.LockedCount++
		}
	}
	if res.LockedCount > 0 {
		if err := e.store.Save(); err != nil {
			return res, err
		}
	}
	return res, nil
}

func (e *windowsEngine) Unlock(id, path, password string) (*core.PublicDirEntry, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	entry := e.findManagedLocked(id, path)
	if entry == nil {
		return nil, errors.New("未找到该目录的锁定记录")
	}
	if err := e.verifyThrottled("密码错误", func() bool {
		return e.verifyEntryPasswordLocked(entry, password)
	}); err != nil {
		return nil, err
	}
	if entry.Locked {
		if err := WinSmartUnlock(entry); err != nil {
			return nil, err
		}
		if err := e.store.Save(); err != nil {
			return nil, err
		}
	}
	pub := entry.ToPublic()
	return &pub, nil
}

func (e *windowsEngine) ChangeDirPassword(id, currentPassword string, useCustom bool, newCustomPassword string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return err
	}
	entry := e.findManagedLocked(id, "")
	if entry == nil {
		return errors.New("未找到该目录记录")
	}
	if err := e.verifyThrottled("当前密码验证失败", func() bool {
		return e.verifyEntryPasswordLocked(entry, currentPassword)
	}); err != nil {
		return err
	}
	if useCustom {
		if len(newCustomPassword) < 4 {
			return errors.New("独立密码长度至少需要 4 个字符")
		}
		h, err := core.HashPassword(newCustomPassword, core.DefaultPBKDF2Iter)
		if err != nil {
			return err
		}
		entry.UseCustomPassword = true
		entry.CustomHash = h
	} else {
		entry.UseCustomPassword = false
		entry.CustomHash = ""
	}
	return e.store.Save()
}

func (e *windowsEngine) Remove(id, password string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return err
	}
	entry := e.findManagedLocked(id, "")
	if entry == nil {
		return errors.New("未找到该目录记录")
	}
	if err := e.verifyThrottled("密码错误，拒绝移出保护列表", func() bool {
		return e.verifyEntryPasswordLocked(entry, password)
	}); err != nil {
		return err
	}
	if entry.Locked {
		if err := WinSmartUnlock(entry); err != nil {
			return err
		}
	}
	e.store.RemoveByID(entry.ID)
	return e.store.Save()
}

func (e *windowsEngine) Diagnose(_ string) (*core.HealthReport, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	return WinDiagnoseState(e.store, e.stateDir), nil
}

func (e *windowsEngine) Repair(_ string, cleanDeadRecords bool) (*core.RepairResult, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return nil, err
	}
	return WinRepairState(e.store, e.stateDir, cleanDeadRecords)
}

func (e *windowsEngine) OpenFolder(path string) error {
	return shellExecuteOpen(path)
}

func (e *windowsEngine) ReadStubFile(stubPath string) (string, string, bool) {
	return WinReadDlockFile(stubPath)
}

func (e *windowsEngine) TryUpgradeInPlace() int {
	e.mu.Lock()
	defer e.mu.Unlock()
	if err := e.reloadLocked(); err != nil {
		return 0
	}
	upgraded := 0
	for i := range e.store.State.Entries {
		if WinTryUpgradeInPlaceEntry(&e.store.State.Entries[i], e.stateDir) {
			upgraded++
		}
	}
	if upgraded > 0 {
		_ = e.store.Save()
	}
	return upgraded
}

func (e *windowsEngine) EnsureShellIntegration() {
	WinEnsureDlockAssociation(getSelfExePath())
}

func (e *windowsEngine) HandleMaintenanceCLI(args []string) (bool, int) {
	exe := getSelfExePath()
	for _, a := range args {
		switch a {
		case "--quit-silent":
			SendQuitSilentToRunningInstance()
			return true, 0
		case "--register", "--install":
			if err := WinInstallIntegration(exe, true); err != nil {
				return true, 1
			}
			return true, 0
		case "--register-no-autostart":
			if err := WinInstallIntegration(exe, false); err != nil {
				return true, 1
			}
			return true, 0
		case "--unregister", "--uninstall":
			WinUninstallIntegration()
			return true, 0
		case "--uninstall-confirm":
			if !winConfirmUninstallIfLocked(e.store.Path) {
				return true, 1
			}
			return true, 0
		}
	}
	return false, 0
}
