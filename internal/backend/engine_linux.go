//go:build linux

package backend

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"time"

	"dirlocker/internal/core"
)

const linuxDaemonSock = "/run/dirlocker/dirlockerd.sock"

type linuxEngine struct{}

func NewEngine() Engine {
	return &linuxEngine{}
}

func (e *linuxEngine) StatePath() string {
	return fmt.Sprintf("/var/lib/dirlocker/uid_%d.json", os.Getuid())
}

type daemonRespBase struct {
	OK      bool   `json:"ok"`
	Message string `json:"message"`
}

func (e *linuxEngine) callDaemon(action string, params map[string]any, timeout time.Duration) ([]byte, error) {
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	conn, err := net.DialTimeout("unix", linuxDaemonSock, 2*time.Second)
	if err != nil {
		return nil, fmt.Errorf("无法连接后台服务 dirlockerd（请确认已执行 sudo systemctl start dirlockerd）: %w", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(timeout))

	payload := make(map[string]any, len(params)+1)
	for k, v := range params {
		payload[k] = v
	}
	payload["action"] = action
	line, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	line = append(line, '\n')
	if _, err := conn.Write(line); err != nil {
		return nil, fmt.Errorf("发送请求失败: %w", err)
	}
	reader := bufio.NewReaderSize(conn, 64*1024)
	respLine, err := reader.ReadBytes('\n')
	if err != nil && len(respLine) == 0 {
		return nil, fmt.Errorf("接收后台服务响应失败: %w", err)
	}
	return respLine, nil
}

func (e *linuxEngine) resolvePathByID(id string) (string, error) {
	if id == "" {
		return "", errors.New("缺少目录标识")
	}
	raw, err := e.callDaemon("query", map[string]any{"id": id}, 5*time.Second)
	if err != nil {
		return "", err
	}
	var resp struct {
		daemonRespBase
		Managed       bool   `json:"managed"`
		CanonicalPath string `json:"canonical_path"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return "", err
	}
	if !resp.OK || !resp.Managed || resp.CanonicalPath == "" {
		if resp.Message != "" {
			return "", errors.New(resp.Message)
		}
		return "", errors.New("未找到该目录记录")
	}
	return resp.CanonicalPath, nil
}

func (e *linuxEngine) Status() (*core.StatusResponse, error) {
	raw, err := e.callDaemon("status", nil, 5*time.Second)
	if err != nil {
		return &core.StatusResponse{
			Settings:        core.DefaultSettings(),
			Entries:         []core.PublicDirEntry{},
			DaemonConnected: false,
			DaemonError:     err.Error(),
		}, nil
	}
	var resp struct {
		daemonRespBase
		HasMasterPassword bool                  `json:"has_master_password"`
		Settings          core.Settings         `json:"settings"`
		Entries           []core.PublicDirEntry `json:"entries"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return &core.StatusResponse{
			Settings:        core.DefaultSettings(),
			Entries:         []core.PublicDirEntry{},
			DaemonConnected: false,
			DaemonError:     resp.Message,
		}, nil
	}
	if resp.Entries == nil {
		resp.Entries = []core.PublicDirEntry{}
	}
	if resp.Settings.Hotkey == "" {
		resp.Settings.Hotkey = core.DefaultHotkey
	}
	return &core.StatusResponse{
		HasMasterPassword: resp.HasMasterPassword,
		Settings:          resp.Settings,
		Entries:           resp.Entries,
		DaemonConnected:   true,
	}, nil
}

func (e *linuxEngine) Query(path string) (*core.QueryResponse, error) {
	raw, err := e.callDaemon("query", map[string]any{"path": path}, 5*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		Managed           bool   `json:"managed"`
		CanonicalPath     string `json:"canonical_path"`
		Locked            bool   `json:"locked"`
		UseCustomPassword bool   `json:"use_custom_password"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, errors.New(resp.Message)
	}
	out := &core.QueryResponse{
		Managed:           resp.Managed,
		Path:              resp.CanonicalPath,
		Locked:            resp.Locked,
		UseCustomPassword: resp.UseCustomPassword,
	}
	if resp.Managed && resp.CanonicalPath != "" {
		if st, err := e.Status(); err == nil {
			for _, ent := range st.Entries {
				if core.PathsEqual(ent.Path, resp.CanonicalPath) {
					out.ID = ent.ID
					break
				}
			}
		}
	}
	return out, nil
}

func (e *linuxEngine) SetMasterPassword(oldPassword, newPassword string) error {
	raw, err := e.callDaemon("set_master_password", map[string]any{
		"old_password": oldPassword,
		"new_password": newPassword,
	}, 10*time.Second)
	if err != nil {
		return err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Message)
	}
	return nil
}

func (e *linuxEngine) VerifyMaster(password string) (bool, error) {
	raw, err := e.callDaemon("verify_master", map[string]any{"password": password}, 10*time.Second)
	if err != nil {
		return false, err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return false, err
	}
	if !resp.OK {
		return false, errors.New(resp.Message)
	}
	return true, nil
}

func (e *linuxEngine) UpdateSettings(password string, settings core.Settings) error {
	raw, err := e.callDaemon("update_settings", map[string]any{
		"password":               password,
		"auto_relock_minutes":    settings.AutoRelockMinutes,
		"require_master_on_open": settings.RequireMasterOnOpen,
		"hotkey":                 settings.Hotkey,
	}, 10*time.Second)
	if err != nil {
		return err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Message)
	}
	return nil
}

func (e *linuxEngine) AddAndLock(path string, useCustom bool, customPassword string) (*core.PublicDirEntry, error) {
	raw, err := e.callDaemon("add_and_lock", map[string]any{
		"path":                path,
		"use_custom_password": useCustom,
		"password":            customPassword,
	}, 20*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		Entries []core.PublicDirEntry `json:"entries"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, errors.New(resp.Message)
	}
	for i := range resp.Entries {
		if core.PathsEqual(resp.Entries[i].Path, path) {
			return &resp.Entries[i], nil
		}
	}
	if len(resp.Entries) > 0 {
		return &resp.Entries[len(resp.Entries)-1], nil
	}
	return &core.PublicDirEntry{Path: path, Locked: true, UseCustomPassword: useCustom}, nil
}

func (e *linuxEngine) Lock(id, path string) error {
	target := path
	if target == "" && id != "" {
		var err error
		target, err = e.resolvePathByID(id)
		if err != nil {
			return err
		}
	}
	raw, err := e.callDaemon("lock", map[string]any{"path": target}, 20*time.Second)
	if err != nil {
		return err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Message)
	}
	return nil
}

func (e *linuxEngine) LockAll() (*core.LockAllResult, error) {
	raw, err := e.callDaemon("lock_all", nil, 30*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		LockedCount  int      `json:"locked_count"`
		SkippedCount int      `json:"skipped_count"`
		SkippedItems []string `json:"skipped_items"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	res := &core.LockAllResult{
		LockedCount:   resp.LockedCount,
		FailedCount:   resp.SkippedCount,
		FailedDetails: resp.SkippedItems,
	}
	if !resp.OK && resp.SkippedCount == 0 && resp.Message != "" {
		return res, errors.New(resp.Message)
	}
	return res, nil
}

func (e *linuxEngine) Unlock(id, path, password string) (*core.PublicDirEntry, error) {
	target := path
	if target == "" && id != "" {
		var err error
		target, err = e.resolvePathByID(id)
		if err != nil {
			return nil, err
		}
	}
	raw, err := e.callDaemon("unlock", map[string]any{
		"path":     target,
		"password": password,
	}, 20*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		Entries []core.PublicDirEntry `json:"entries"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, errors.New(resp.Message)
	}
	for i := range resp.Entries {
		if (id != "" && resp.Entries[i].ID == id) || core.PathsEqual(resp.Entries[i].Path, target) {
			return &resp.Entries[i], nil
		}
	}
	return &core.PublicDirEntry{ID: id, Path: target, Locked: false}, nil
}

func (e *linuxEngine) ChangeDirPassword(id, currentPassword string, useCustom bool, newCustomPassword string) error {
	target, err := e.resolvePathByID(id)
	if err != nil {
		return err
	}
	raw, err := e.callDaemon("change_dir_password", map[string]any{
		"path":                target,
		"current_password":    currentPassword,
		"use_custom_password": useCustom,
		"new_password":        newCustomPassword,
	}, 10*time.Second)
	if err != nil {
		return err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Message)
	}
	return nil
}

func (e *linuxEngine) Remove(id, password string) error {
	target, err := e.resolvePathByID(id)
	if err != nil {
		return err
	}
	raw, err := e.callDaemon("remove", map[string]any{
		"path":     target,
		"password": password,
	}, 20*time.Second)
	if err != nil {
		return err
	}
	var resp daemonRespBase
	if err := json.Unmarshal(raw, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Message)
	}
	return nil
}

func (e *linuxEngine) Diagnose(password string) (*core.HealthReport, error) {
	raw, err := e.callDaemon("diagnose", map[string]any{"password": password}, 20*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		core.HealthReport
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, errors.New(resp.Message)
	}
	if resp.Items == nil {
		resp.Items = []core.HealthItem{}
	}
	return &resp.HealthReport, nil
}

func (e *linuxEngine) Repair(password string, cleanDeadRecords bool) (*core.RepairResult, error) {
	raw, err := e.callDaemon("repair", map[string]any{
		"password":           password,
		"clean_dead_records": cleanDeadRecords,
	}, 30*time.Second)
	if err != nil {
		return nil, err
	}
	var resp struct {
		daemonRespBase
		RepairedCount int      `json:"repaired_count"`
		Details       []string `json:"details"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, errors.New(resp.Message)
	}
	if resp.Details == nil {
		resp.Details = []string{}
	}
	return &core.RepairResult{
		RepairedCount: resp.RepairedCount,
		Details:       resp.Details,
	}, nil
}

// OpenFolder launches Kylin's native Peony file manager directly to avoid third-party MIME hijacking.
func (e *linuxEngine) OpenFolder(path string) error {
	if peony, err := exec.LookPath("peony"); err == nil && peony != "" {
		return exec.Command(peony, path).Start()
	}
	if fm, err := exec.LookPath("xdg-open"); err == nil && fm != "" {
		return exec.Command(fm, path).Start()
	}
	return errors.New("未找到可用的文件管理器 (peony / xdg-open)")
}

func (e *linuxEngine) ReadStubFile(stubPath string) (string, string, bool) {
	raw, err := os.ReadFile(stubPath)
	if err != nil {
		return "", "", false
	}
	var id, origPath string
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "X-DirLocker-ID=") {
			id = strings.TrimSpace(strings.TrimPrefix(line, "X-DirLocker-ID="))
		} else if strings.HasPrefix(line, "X-DirLocker-Path=") {
			origPath = strings.TrimSpace(strings.TrimPrefix(line, "X-DirLocker-Path="))
		} else if id == "" && strings.HasPrefix(line, "Exec=") {
			if idx := strings.Index(line, "--unlock-id "); idx >= 0 {
				rest := strings.TrimSpace(line[idx+len("--unlock-id "):])
				fields := strings.Fields(rest)
				if len(fields) > 0 {
					id = strings.Trim(fields[0], `"'`)
				}
			}
		}
	}
	return id, origPath, id != "" || origPath != ""
}

func (e *linuxEngine) TryUpgradeInPlace() int { return 0 }

func (e *linuxEngine) EnsureShellIntegration() {}

func (e *linuxEngine) HandleMaintenanceCLI(args []string) (bool, int) {
	for _, a := range args {
		if a == "--quit-silent" {
			// Let SingleInstanceLock forward --quit-silent if an instance is running,
			// or exit immediately if not running. We return false here only if we want
			// Wails SingleInstanceLock to deliver it; however on Linux preinst also runs pkill.
			// Exiting cleanly here avoids spawning a GTK window when no instance is running.
			return true, 0
		}
	}
	return false, 0
}
