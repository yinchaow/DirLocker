package core

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

type StateStore struct {
	Path  string
	State State
}

func NewStateStore(path string) *StateStore {
	return &StateStore{
		Path: path,
		State: State{
			Version:  StateFormatVer,
			Settings: DefaultSettings(),
			Entries:  []DirEntry{},
		},
	}
}

func (s *StateStore) BackupPath() string {
	return s.Path + ".bak"
}

func (s *StateStore) HasMasterPassword() bool {
	return s.State.MasterHash != ""
}

func (s *StateStore) Load() error {
	primaryData, primaryErr := os.ReadFile(s.Path)
	if primaryErr == nil {
		if err := s.parseJSON(primaryData); err == nil {
			return nil
		}
	}
	if bakData, bakErr := os.ReadFile(s.BackupPath()); bakErr == nil {
		if err := s.parseJSON(bakData); err == nil {
			_ = s.Save()
			return nil
		}
	}
	if os.IsNotExist(primaryErr) {
		s.State = State{
			Version:  StateFormatVer,
			Settings: DefaultSettings(),
			Entries:  []DirEntry{},
		}
		return nil
	}
	return fmt.Errorf("读取状态文件失败: %w", primaryErr)
}

func (s *StateStore) parseJSON(raw []byte) error {
	st := State{
		Version:  StateFormatVer,
		Settings: DefaultSettings(),
		Entries:  []DirEntry{},
	}
	if err := json.Unmarshal(raw, &st); err != nil {
		return err
	}
	if st.Version <= 0 {
		st.Version = StateFormatVer
	}
	if st.Settings.AutoRelockMinutes < 0 {
		st.Settings.AutoRelockMinutes = 0
	}
	if st.Settings.AutoRelockMinutes > 1440 {
		st.Settings.AutoRelockMinutes = 1440
	}
	migrated := false
	if st.Settings.Hotkey == "" || st.Settings.Hotkey == LegacyHotkey {
		st.Settings.Hotkey = DefaultHotkey
		migrated = true
	} else {
		norm := NormalizeHotkey(st.Settings.Hotkey, DefaultHotkey)
		if norm != st.Settings.Hotkey {
			st.Settings.Hotkey = norm
			migrated = true
		}
	}
	if st.Entries == nil {
		st.Entries = []DirEntry{}
	}
	s.State = st
	if migrated {
		_ = s.Save()
	}
	return nil
}

func (s *StateStore) Save() error {
	dir := filepath.Dir(s.Path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return fmt.Errorf("创建状态目录失败: %w", err)
	}
	s.State.Version = StateFormatVer
	if s.State.Entries == nil {
		s.State.Entries = []DirEntry{}
	}
	raw, err := json.MarshalIndent(s.State, "", "  ")
	if err != nil {
		return fmt.Errorf("序列化状态失败: %w", err)
	}
	raw = append(raw, '\n')

	tmpPath := s.Path + ".tmp"
	if err := writeFileAtomic(tmpPath, s.Path, raw); err != nil {
		return fmt.Errorf("写入状态文件失败: %w", err)
	}
	bakTmp := s.BackupPath() + ".tmp"
	_ = writeFileAtomic(bakTmp, s.BackupPath(), raw)
	return nil
}

func writeFileAtomic(tmpPath, finalPath string, data []byte) error {
	f, err := os.OpenFile(tmpPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		_ = f.Close()
		_ = os.Remove(tmpPath)
		return err
	}
	_ = f.Sync()
	if err := f.Close(); err != nil {
		_ = os.Remove(tmpPath)
		return err
	}
	// On Windows os.Rename may fail if target exists and is read-only; remove/replace safely.
	if err := os.Rename(tmpPath, finalPath); err != nil {
		_ = os.Chmod(finalPath, 0600)
		_ = os.Remove(finalPath)
		if err2 := os.Rename(tmpPath, finalPath); err2 != nil {
			_ = os.Remove(tmpPath)
			return err2
		}
	}
	return nil
}

func (s *StateStore) FindByID(id string) *DirEntry {
	for i := range s.State.Entries {
		if s.State.Entries[i].ID == id {
			return &s.State.Entries[i]
		}
	}
	return nil
}

func (s *StateStore) FindByPath(p string) *DirEntry {
	for i := range s.State.Entries {
		if PathsEqual(s.State.Entries[i].Path, p) {
			return &s.State.Entries[i]
		}
	}
	return nil
}

func (s *StateStore) RemoveByID(id string) bool {
	for i := range s.State.Entries {
		if s.State.Entries[i].ID == id {
			s.State.Entries = append(s.State.Entries[:i], s.State.Entries[i+1:]...)
			return true
		}
	}
	return false
}

func NowEpoch() int64 {
	return time.Now().Unix()
}
