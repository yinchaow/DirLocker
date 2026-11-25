package main

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"dirlocker/internal/backend"
	"dirlocker/internal/core"
	"github.com/wailsapp/wails/v2/pkg/options"
	wailsRuntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

type App struct {
	ctx        context.Context
	engine     backend.Engine
	hooks      backend.PlatformHooks
	launchArgs []string
	startMode  string // "normal", "tray", "post-install"

	mu            sync.Mutex
	hotkeyActive  bool
	hotkeyWarning string
	forceQuit     bool
	stopBg        chan struct{}
	pendingAction *core.ActionRequestEvent
}

func NewApp(eng backend.Engine, hooks backend.PlatformHooks, args []string, startMode string) *App {
	return &App{
		engine:     eng,
		hooks:      hooks,
		launchArgs: args,
		startMode:  startMode,
		stopBg:     make(chan struct{}),
	}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.engine.EnsureShellIntegration()

	a.hooks.Start(
		func() { a.showMainWindow() },
		func() { a.triggerHotkeyLockAll() },
		func() { a.requestQuitFromTray() },
	)

	// Register configured global hotkey
	if st, err := a.engine.Status(); err == nil && st.DaemonConnected {
		hk := st.Settings.Hotkey
		if hk == "" {
			hk = core.DefaultHotkey
		}
		ok, warn := a.hooks.RegisterHotkey(hk)
		a.mu.Lock()
		a.hotkeyActive = ok
		a.hotkeyWarning = warn
		a.mu.Unlock()
	}

	// Queue any initial CLI action (--unlock-file, --unlock-id, --toggle, --lock, --unlock, --post-install)
	a.parseInitialAction(a.launchArgs)

	go a.runBackgroundLoops()
}

func (a *App) domReady(ctx context.Context) {
	a.ctx = ctx
	a.hooks.ApplyWindowRoundedCorners()

	a.mu.Lock()
	act := a.pendingAction
	a.pendingAction = nil
	a.mu.Unlock()

	if act != nil {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:action", act)
	}
}

func (a *App) beforeClose(ctx context.Context) (prevent bool) {
	a.mu.Lock()
	force := a.forceQuit
	a.mu.Unlock()
	if force || a.startMode == "post-install" {
		return false
	}
	if a.hooks.TrayAvailable() {
		wailsRuntime.WindowHide(ctx)
		wailsRuntime.EventsEmit(ctx, "dirlocker:hidden-to-tray", true)
		return true
	}
	// No system tray available: ask frontend to confirm if any dirs are unlocked
	if st, err := a.engine.Status(); err == nil {
		unlocked := 0
		for _, e := range st.Entries {
			if !e.Locked {
				unlocked++
			}
		}
		if unlocked > 0 {
			wailsRuntime.EventsEmit(ctx, "dirlocker:action", &core.ActionRequestEvent{
				Type:    "quit-confirm",
				Message: fmt.Sprintf("当前仍有 %d 个目录处于已解锁状态", unlocked),
			})
			return true
		}
	}
	return false
}

func (a *App) shutdown(ctx context.Context) {
	select {
	case <-a.stopBg:
	default:
		close(a.stopBg)
	}
	a.hooks.Stop()
}

func (a *App) showMainWindow() {
	if a.ctx == nil {
		return
	}
	wailsRuntime.WindowShow(a.ctx)
	wailsRuntime.WindowUnminimise(a.ctx)
	wailsRuntime.WindowSetAlwaysOnTop(a.ctx, true)
	wailsRuntime.WindowSetAlwaysOnTop(a.ctx, false)
	a.hooks.ApplyWindowRoundedCorners()
	wailsRuntime.EventsEmit(a.ctx, "dirlocker:shown", true)
}

func (a *App) triggerHotkeyLockAll() {
	res, err := a.engine.LockAll()
	if a.ctx == nil {
		return
	}
	if err != nil {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
			"tone":    "error",
			"message": "快捷键一键全锁失败：" + err.Error(),
		})
		return
	}
	a.emitStatusRefresh()
	if res.FailedCount > 0 {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
			"tone":    "error",
			"message": fmt.Sprintf("已锁定 %d 个目录，%d 个因被占用跳过", res.LockedCount, res.FailedCount),
		})
	} else if res.LockedCount > 0 {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
			"tone":    "success",
			"message": fmt.Sprintf("一键全锁完成：已锁定 %d 个目录", res.LockedCount),
		})
	} else {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
			"tone":    "info",
			"message": "所有受管目录均已处于锁定状态",
		})
	}
}

func (a *App) requestQuitFromTray() {
	if a.ctx == nil {
		os.Exit(0)
		return
	}
	st, err := a.engine.Status()
	if err == nil {
		unlocked := 0
		for _, e := range st.Entries {
			if !e.Locked {
				unlocked++
			}
		}
		if unlocked > 0 {
			a.showMainWindow()
			wailsRuntime.EventsEmit(a.ctx, "dirlocker:action", &core.ActionRequestEvent{
				Type:    "quit-confirm",
				Message: fmt.Sprintf("当前仍有 %d 个目录处于已解锁状态", unlocked),
			})
			return
		}
	}
	a.ForceQuit(false)
}

func (a *App) parseInitialAction(args []string) {
	act := a.buildActionFromArgs(args)
	if act != nil {
		a.mu.Lock()
		a.pendingAction = act
		a.mu.Unlock()
	}
}

func (a *App) buildActionFromArgs(args []string) *core.ActionRequestEvent {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		next := ""
		if i+1 < len(args) {
			next = args[i+1]
		}
		switch arg {
		case "--post-install":
			return &core.ActionRequestEvent{Type: "post-install"}
		case "--lock-all":
			go a.triggerHotkeyLockAll()
			return nil
		case "--unlock-id":
			if next != "" {
				return &core.ActionRequestEvent{Type: "unlock", EntryID: next}
			}
		case "--unlock-file":
			if next != "" {
				id, origPath, ok := a.engine.ReadStubFile(next)
				if ok {
					return &core.ActionRequestEvent{Type: "unlock", EntryID: id, Path: origPath}
				}
				return &core.ActionRequestEvent{Type: "unlock", Path: next}
			}
		case "--unlock":
			if next != "" {
				return &core.ActionRequestEvent{Type: "unlock", Path: next}
			}
		case "--lock":
			if next != "" {
				return &core.ActionRequestEvent{Type: "lock", Path: next}
			}
		case "--toggle":
			if next != "" {
				q, err := a.engine.Query(next)
				if err == nil && q.Managed {
					if q.Locked {
						return &core.ActionRequestEvent{Type: "unlock", EntryID: q.ID, Path: q.Path}
					}
					return &core.ActionRequestEvent{Type: "lock", EntryID: q.ID, Path: q.Path}
				}
				return &core.ActionRequestEvent{Type: "lock", Path: next}
			}
		}
	}
	// Positional .dlock / .desktop file passed directly
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			continue
		}
		lower := strings.ToLower(arg)
		if strings.HasSuffix(lower, ".dlock") || strings.HasSuffix(lower, ".desktop") {
			id, origPath, ok := a.engine.ReadStubFile(arg)
			if ok {
				return &core.ActionRequestEvent{Type: "unlock", EntryID: id, Path: origPath}
			}
			return &core.ActionRequestEvent{Type: "unlock", Path: arg}
		}
	}
	return nil
}

func (a *App) onSecondInstanceLaunch(data options.SecondInstanceData) {
	args := data.Args
	for _, arg := range args {
		if arg == "--quit-silent" {
			a.ForceQuit(false)
			return
		}
	}
	for _, arg := range args {
		if arg == "--lock-all" {
			go a.triggerHotkeyLockAll()
			return
		}
	}

	act := a.buildActionFromArgs(args)
	a.showMainWindow()
	if act != nil && a.ctx != nil {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:action", act)
	}
}

func (a *App) runBackgroundLoops() {
	idleTicker := time.NewTicker(5 * time.Second)
	upgradeTicker := time.NewTicker(15 * time.Second)
	defer idleTicker.Stop()
	defer upgradeTicker.Stop()

	for {
		select {
		case <-a.stopBg:
			return
		case <-idleTicker.C:
			st, err := a.engine.Status()
			if err != nil || !st.DaemonConnected {
				continue
			}
			mins := st.Settings.AutoRelockMinutes
			if mins <= 0 {
				continue
			}
			hasUnlocked := false
			for _, e := range st.Entries {
				if !e.Locked {
					hasUnlocked = true
					break
				}
			}
			if !hasUnlocked {
				continue
			}
			idleSec, ok := a.hooks.IdleSeconds()
			if !ok || idleSec < mins*60 {
				continue
			}
			if res, err := a.engine.LockAll(); err == nil && res.LockedCount > 0 {
				a.emitStatusRefresh()
				if a.ctx != nil {
					wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
						"tone":    "info",
						"message": fmt.Sprintf("系统已闲置超过 %d 分钟，已自动回锁 %d 个目录", mins, res.LockedCount),
					})
				}
			}
		case <-upgradeTicker.C:
			if n := a.engine.TryUpgradeInPlace(); n > 0 {
				a.emitStatusRefresh()
				if a.ctx != nil {
					wailsRuntime.EventsEmit(a.ctx, "dirlocker:toast", map[string]string{
						"tone":    "success",
						"message": fmt.Sprintf("原位占用的 %d 个目录已自动升级为完整保护区锁定", n),
					})
				}
			}
		}
	}
}

func (a *App) emitStatusRefresh() {
	if a.ctx == nil {
		return
	}
	if st, err := a.GetStatus(); err == nil {
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:state", st)
	}
}

// ==================== Wails Bound Methods ====================

func (a *App) GetSystemInfo() core.SystemInfo {
	home, _ := os.UserHomeDir()
	return core.SystemInfo{
		OS:            runtime.GOOS,
		Arch:          runtime.GOARCH,
		AppVersion:    core.AppVersion,
		StatePath:     a.engine.StatePath(),
		HomeDir:       home,
		TrayAvailable: a.hooks.TrayAvailable(),
		StartMode:     a.startMode,
	}
}

func (a *App) GetStatus() (*core.StatusResponse, error) {
	st, err := a.engine.Status()
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	st.HotkeyActive = a.hotkeyActive
	st.HotkeyWarning = a.hotkeyWarning
	a.mu.Unlock()
	st.TrayAvailable = a.hooks.TrayAvailable()
	return st, nil
}

func (a *App) SelectDirectory(defaultDir string) (string, error) {
	if defaultDir == "" {
		defaultDir, _ = os.UserHomeDir()
	}
	return wailsRuntime.OpenDirectoryDialog(a.ctx, wailsRuntime.OpenDialogOptions{
		Title:            "选择要锁定保护的目录",
		DefaultDirectory: defaultDir,
	})
}

func (a *App) FilterDirectories(paths []string) []string {
	return backend.FilterDirectories(paths)
}

func (a *App) SetMasterPassword(oldPassword, newPassword string) (*core.StatusResponse, error) {
	if err := a.engine.SetMasterPassword(oldPassword, newPassword); err != nil {
		return nil, err
	}
	return a.GetStatus()
}

func (a *App) VerifyMaster(password string) (bool, error) {
	return a.engine.VerifyMaster(password)
}

func (a *App) UpdateSettings(password string, settings core.Settings) (*core.StatusResponse, error) {
	if err := a.engine.UpdateSettings(password, settings); err != nil {
		return nil, err
	}
	ok, warn := a.hooks.RegisterHotkey(settings.Hotkey)
	a.mu.Lock()
	a.hotkeyActive = ok
	a.hotkeyWarning = warn
	a.mu.Unlock()
	return a.GetStatus()
}

type HotkeyTestResult struct {
	OK      bool   `json:"ok"`
	Message string `json:"message"`
}

func (a *App) TestHotkey(hotkey string) HotkeyTestResult {
	ok, msg := a.hooks.TestHotkey(hotkey)
	return HotkeyTestResult{OK: ok, Message: msg}
}

func (a *App) AddAndLock(path string, useCustom bool, customPassword string) (*core.StatusResponse, error) {
	if _, err := a.engine.AddAndLock(path, useCustom, customPassword); err != nil {
		return nil, err
	}
	return a.GetStatus()
}

func (a *App) LockDir(id, path string) (*core.StatusResponse, error) {
	if err := a.engine.Lock(id, path); err != nil {
		return nil, err
	}
	return a.GetStatus()
}

func (a *App) LockAll() (*core.LockAllResult, error) {
	res, err := a.engine.LockAll()
	a.emitStatusRefresh()
	return res, err
}

func (a *App) UnlockDir(id, path, password string, openAfter bool) (*core.StatusResponse, error) {
	ent, err := a.engine.Unlock(id, path, password)
	if err != nil {
		return nil, err
	}
	if openAfter && ent != nil && ent.Path != "" {
		_ = a.engine.OpenFolder(ent.Path)
	}
	return a.GetStatus()
}

func (a *App) ChangeDirPassword(id, currentPassword string, useCustom bool, newCustomPassword string) (*core.StatusResponse, error) {
	if err := a.engine.ChangeDirPassword(id, currentPassword, useCustom, newCustomPassword); err != nil {
		return nil, err
	}
	return a.GetStatus()
}

func (a *App) RemoveDir(id, password string) (*core.StatusResponse, error) {
	if err := a.engine.Remove(id, password); err != nil {
		return nil, err
	}
	return a.GetStatus()
}

func (a *App) Diagnose(password string) (*core.HealthReport, error) {
	return a.engine.Diagnose(password)
}

func (a *App) Repair(password string, cleanDeadRecords bool) (*core.RepairResult, error) {
	res, err := a.engine.Repair(password, cleanDeadRecords)
	a.emitStatusRefresh()
	return res, err
}

func (a *App) OpenFolder(path string) error {
	return a.engine.OpenFolder(path)
}

func (a *App) OpenURL(u string) error {
	return a.hooks.OpenURL(u)
}

func (a *App) SyncWindowCorners() {
	a.hooks.ApplyWindowRoundedCorners()
}

func (a *App) HideToTrayOrQuit() {
	if a.hooks.TrayAvailable() {
		wailsRuntime.WindowHide(a.ctx)
		wailsRuntime.EventsEmit(a.ctx, "dirlocker:hidden-to-tray", true)
		return
	}
	a.requestQuitFromTray()
}

func (a *App) CompletePostInstall(launchMain bool) {
	if !launchMain {
		a.ForceQuit(false)
		return
	}
	a.startMode = "normal"
	wailsRuntime.WindowSetSize(a.ctx, 1040, 720)
	wailsRuntime.WindowCenter(a.ctx)
	a.hooks.ApplyWindowRoundedCorners()
}

func (a *App) ForceQuit(relockAll bool) {
	if relockAll {
		_, _ = a.engine.LockAll()
	}
	a.mu.Lock()
	a.forceQuit = true
	a.mu.Unlock()
	if a.ctx != nil {
		wailsRuntime.Quit(a.ctx)
	} else {
		os.Exit(0)
	}
}
