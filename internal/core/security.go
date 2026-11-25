package core

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

type HotkeySpec struct {
	Ctrl  bool
	Alt   bool
	Shift bool
	Key   string // "A"-"Z", "0"-"9", "F1"-"F12"
}

func (h HotkeySpec) IsValid() bool {
	return (h.Ctrl || h.Alt) && h.Key != ""
}

func (h HotkeySpec) String() string {
	if !h.IsValid() {
		return ""
	}
	var b strings.Builder
	if h.Ctrl {
		b.WriteString("Ctrl+")
	}
	if h.Alt {
		b.WriteString("Alt+")
	}
	if h.Shift {
		b.WriteString("Shift+")
	}
	b.WriteString(h.Key)
	return b.String()
}

func normalizeMainKey(upper string) (string, bool) {
	if len(upper) == 1 {
		c := upper[0]
		if (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') {
			return upper, true
		}
		return "", false
	}
	if len(upper) >= 2 && len(upper) <= 3 && upper[0] == 'F' {
		fn, err := strconv.Atoi(upper[1:])
		if err == nil && fn >= 1 && fn <= 12 {
			return fmt.Sprintf("F%d", fn), true
		}
	}
	return "", false
}

func ParseHotkey(raw string) (HotkeySpec, error) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return HotkeySpec{}, errors.New("快捷键不能为空")
	}
	parts := strings.Split(trimmed, "+")
	var out HotkeySpec
	for _, p := range parts {
		tok := strings.ToUpper(strings.TrimSpace(p))
		if tok == "" {
			return HotkeySpec{}, errors.New("快捷键格式无效（存在空键段）")
		}
		switch tok {
		case "CTRL", "CONTROL":
			out.Ctrl = true
		case "ALT":
			out.Alt = true
		case "SHIFT":
			out.Shift = true
		default:
			if out.Key != "" {
				return HotkeySpec{}, errors.New("快捷键只能包含一个主键")
			}
			norm, ok := normalizeMainKey(tok)
			if !ok {
				return HotkeySpec{}, errors.New("主键仅支持字母 A-Z、数字 0-9 或功能键 F1-F12")
			}
			out.Key = norm
		}
	}
	if !out.Ctrl && !out.Alt {
		return HotkeySpec{}, errors.New("全局快捷键必须至少包含 Ctrl 或 Alt 修饰键")
	}
	if out.Key == "" {
		return HotkeySpec{}, errors.New("请按下字母、数字或 F1-F12 主键完成组合")
	}
	if out.Alt && !out.Ctrl && !out.Shift && out.Key == "F4" {
		return HotkeySpec{}, errors.New("Alt+F4 为系统关闭窗口快捷键，请更换其他组合")
	}
	return out, nil
}

func NormalizeHotkey(raw, fallback string) string {
	if spec, err := ParseHotkey(raw); err == nil {
		return spec.String()
	}
	if spec, err := ParseHotkey(fallback); err == nil {
		return spec.String()
	}
	return DefaultHotkey
}

// AuthThrottle protects password verification against brute-force attempts using exponential backoff.
// Never deletes or damages user data.
type AuthThrottle struct {
	mu                  sync.Mutex
	consecutiveFailures int
	cooldownUntil       time.Time
}

func CooldownForFailures(failures int) int {
	if failures < 5 {
		return 0
	}
	if failures == 5 {
		return 30
	}
	if failures == 6 {
		return 60
	}
	return 300
}

func (t *AuthThrottle) RemainingCooldownSeconds() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := time.Now()
	if !t.cooldownUntil.After(now) {
		return 0
	}
	diff := t.cooldownUntil.Sub(now)
	return int((diff + time.Second - 1) / time.Second)
}

func (t *AuthThrottle) RecordFailure() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.consecutiveFailures++
	sec := CooldownForFailures(t.consecutiveFailures)
	if sec > 0 {
		target := time.Now().Add(time.Duration(sec) * time.Second)
		if target.After(t.cooldownUntil) {
			t.cooldownUntil = target
		}
	}
	return sec
}

func (t *AuthThrottle) RecordSuccess() {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.consecutiveFailures = 0
	t.cooldownUntil = time.Time{}
}

func FormatCooldownError(remainingSec int) string {
	if remainingSec <= 0 {
		return ""
	}
	return fmt.Sprintf("连续密码错误次数过多，为防暴力试探，请等待 %d 秒后再试（不会删除或损坏您的目录数据）", remainingSec)
}

// Path normalization and guard helpers

func NormalizePathLexical(p string) string {
	if p == "" {
		return ""
	}
	if runtime.GOOS == "windows" {
		return NormalizeWinPath(p)
	}
	return NormalizeUnixPath(p)
}

func NormalizeWinPath(p string) string {
	s := strings.ReplaceAll(strings.TrimSpace(p), "/", `\`)
	if strings.HasPrefix(s, `\\?\UNC\`) {
		s = `\\` + s[8:]
	} else if strings.HasPrefix(s, `\\?\`) {
		s = s[4:]
	}
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i > 0 && s[i-1] == '\\' {
			if i == 1 {
				b.WriteByte('\\')
			}
			continue
		}
		b.WriteByte(s[i])
	}
	s = b.String()
	for len(s) > 3 && strings.HasSuffix(s, `\`) {
		s = s[:len(s)-1]
	}
	if len(s) == 2 && isAlpha(s[0]) && s[1] == ':' {
		s += `\`
	}
	if len(s) >= 2 && isAlpha(s[0]) && s[1] == ':' {
		s = strings.ToUpper(s[:1]) + s[1:]
	}
	return s
}

func NormalizeUnixPath(p string) string {
	s := strings.TrimSpace(p)
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '/' && b.Len() > 0 && s[i-1] == '/' {
			continue
		}
		b.WriteByte(s[i])
	}
	s = b.String()
	for len(s) > 2 && strings.HasSuffix(s, "/.") {
		s = s[:len(s)-2]
	}
	for len(s) > 1 && strings.HasSuffix(s, "/") {
		s = s[:len(s)-1]
	}
	return s
}

func isAlpha(c byte) bool {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
}

func PathsEqual(a, b string) bool {
	na := NormalizePathLexical(a)
	nb := NormalizePathLexical(b)
	if runtime.GOOS == "windows" {
		return strings.EqualFold(na, nb)
	}
	return na == nb
}

func IsSameOrSubpath(child, parent string) bool {
	c := NormalizePathLexical(child)
	p := NormalizePathLexical(parent)
	if c == "" || p == "" {
		return false
	}
	if runtime.GOOS == "windows" {
		cl := strings.ToLower(c)
		pl := strings.ToLower(p)
		if cl == pl {
			return true
		}
		if strings.HasSuffix(pl, `\`) {
			return len(cl) > len(pl) && strings.HasPrefix(cl, pl)
		}
		return len(cl) > len(pl) && strings.HasPrefix(cl, pl) && cl[len(pl)] == '\\'
	}
	if c == p {
		return true
	}
	if p == "/" {
		return strings.HasPrefix(c, "/")
	}
	return len(c) > len(p) && strings.HasPrefix(c, p) && c[len(p)] == '/'
}

func PathsOverlap(a, b string) bool {
	return IsSameOrSubpath(a, b) || IsSameOrSubpath(b, a)
}

func isDriveRootWin(p string) bool {
	return len(p) == 3 && isAlpha(p[0]) && p[1] == ':' && p[2] == '\\'
}

func isUNCRootWin(p string) bool {
	if len(p) < 5 || !strings.HasPrefix(p, `\\`) {
		return false
	}
	rest := p[2:]
	slash := strings.IndexByte(rest, '\\')
	if slash <= 0 {
		return false
	}
	return !strings.Contains(rest[slash+1:], `\`)
}

func IsProtectedSystemPath(canonicalPath, userHome string, extraProtected []string) (bool, string) {
	norm := NormalizePathLexical(canonicalPath)
	if norm == "" {
		return true, "路径为空"
	}
	if runtime.GOOS == "windows" {
		if isDriveRootWin(norm) || isUNCRootWin(norm) {
			return true, "禁止锁定磁盘根目录或网络共享根目录"
		}
		if len(norm) >= 3 && norm[1] == ':' && norm[2] == '\\' {
			drvVault := norm[:3] + "$目录密匣_重要数据保护区_切勿删除"
			drvVaultLegacy := norm[:3] + "$DirShieldVault"
			if IsSameOrSubpath(norm, drvVault) || IsSameOrSubpath(norm, drvVaultLegacy) {
				return true, "禁止锁定目录密匣系统数据保护区"
			}
		}
		winDir := os.Getenv("WINDIR")
		if winDir == "" {
			winDir = `C:\Windows`
		}
		winDir = NormalizeWinPath(winDir)
		sysDrive := `C:\`
		if len(winDir) >= 3 {
			sysDrive = winDir[:3]
		}
		prefixBlocked := []string{
			winDir,
			sysDrive + `Program Files`,
			sysDrive + `Program Files (x86)`,
			sysDrive + `ProgramData`,
			sysDrive + `Recovery`,
			sysDrive + `$Recycle.Bin`,
			sysDrive + `System Volume Information`,
		}
		for _, envName := range []string{"ProgramFiles", "ProgramFiles(x86)", "ProgramData"} {
			if v := os.Getenv(envName); v != "" {
				prefixBlocked = append(prefixBlocked, NormalizeWinPath(v))
			}
		}
		home := userHome
		if home == "" {
			home = os.Getenv("USERPROFILE")
		}
		home = NormalizeWinPath(home)
		if home != "" {
			prefixBlocked = append(prefixBlocked, home+`\AppData`)
		}
		if localApp := os.Getenv("LOCALAPPDATA"); localApp != "" {
			prefixBlocked = append(prefixBlocked, NormalizeWinPath(localApp)+`\Programs\DirLocker`)
		}
		for _, ex := range extraProtected {
			if ex != "" {
				prefixBlocked = append(prefixBlocked, NormalizeWinPath(ex))
			}
		}
		for _, blk := range prefixBlocked {
			if IsSameOrSubpath(norm, blk) {
				return true, "禁止锁定系统目录或程序受保护目录: " + blk
			}
			if IsSameOrSubpath(blk, norm) {
				return true, "禁止锁定包含系统或受保护路径的上级目录"
			}
		}
		exactBlocked := []string{sysDrive + `Users`}
		if home != "" {
			exactBlocked = append(exactBlocked,
				home,
				home+`\Desktop`,
				home+`\桌面`,
				home+`\Documents`,
				home+`\Downloads`,
			)
		}
		for _, blk := range exactBlocked {
			if PathsEqual(norm, blk) {
				return true, "禁止直接锁定用户主目录或桌面根目录本身（可锁定其子目录）"
			}
		}
		return false, ""
	}

	// Linux / Unix
	if norm == "/" {
		return true, "禁止锁定根目录 /"
	}
	const vaultSuffix = "/.dirlocker_vault_do_not_delete"
	if norm == vaultSuffix || strings.Contains(norm, vaultSuffix+"/") || strings.HasSuffix(norm, vaultSuffix) {
		return true, "禁止锁定目录密匣系统数据保护区"
	}
	prefixBlocked := []string{
		"/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64", "/libx32",
		"/proc", "/run", "/sbin", "/snap", "/sys", "/usr", "/var/lib/dirlocker",
	}
	home := userHome
	if home == "" {
		home, _ = os.UserHomeDir()
	}
	home = NormalizeUnixPath(home)
	if home != "" {
		prefixBlocked = append(prefixBlocked,
			home+"/.config",
			home+"/.local",
			home+"/.cache",
			home+"/.ssh",
			home+"/.gnupg",
		)
	}
	for _, ex := range extraProtected {
		if ex != "" {
			prefixBlocked = append(prefixBlocked, NormalizeUnixPath(ex))
		}
	}
	for _, blk := range prefixBlocked {
		if IsSameOrSubpath(norm, blk) {
			return true, "禁止锁定系统目录或受保护配置目录: " + blk
		}
		if IsSameOrSubpath(blk, norm) {
			return true, "禁止锁定包含系统或受保护目录的上级目录"
		}
	}
	exactBlocked := []string{"/home", "/root", "/tmp", "/var", "/var/lib", "/opt", "/srv", "/media", "/mnt"}
	if home != "" {
		exactBlocked = append(exactBlocked,
			home,
			home+"/Desktop",
			home+"/桌面",
			home+"/Documents",
			home+"/文档",
			home+"/Downloads",
			home+"/下载",
		)
	}
	for _, blk := range exactBlocked {
		if PathsEqual(norm, blk) {
			return true, "禁止直接锁定系统顶层目录、用户主目录或桌面根目录本身（可锁定其子目录）"
		}
	}
	return false, ""
}

func CanonicalizeExistingDir(rawPath string) (string, error) {
	lex := NormalizePathLexical(rawPath)
	if lex == "" {
		return "", errors.New("路径为空")
	}
	info, err := os.Lstat(lex)
	if err != nil {
		return "", errors.New("目录不存在或无法访问")
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return "", errors.New("不支持锁定符号链接或挂载点目录")
	}
	if !info.IsDir() {
		return "", errors.New("目标不是目录")
	}
	abs, err := filepath.Abs(lex)
	if err != nil {
		return "", fmt.Errorf("解析完整路径失败: %w", err)
	}
	return NormalizePathLexical(abs), nil
}

func ValidateLockableDirectory(rawPath string, existingPaths []string, userHome string, extraProtected []string) (string, error) {
	canon, err := CanonicalizeExistingDir(rawPath)
	if err != nil {
		return "", err
	}
	if blocked, reason := IsProtectedSystemPath(canon, userHome, extraProtected); blocked {
		return "", errors.New(reason)
	}
	for _, ex := range existingPaths {
		if PathsEqual(canon, ex) {
			return "", errors.New("该目录已在 目录密匣 管理列表中")
		}
		if PathsOverlap(canon, ex) {
			return "", errors.New("不允许嵌套锁定已受保护目录的上级或下级目录: " + ex)
		}
	}
	return canon, nil
}
