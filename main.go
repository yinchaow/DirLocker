package main

import (
	"embed"
	"log"
	"os"

	"dirlocker/internal/backend"
	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/linux"
	"github.com/wailsapp/wails/v2/pkg/options/windows"
)

//go:embed all:frontend/dist
var assets embed.FS

func main() {
	args := os.Args[1:]
	eng := backend.NewEngine()

	// Handle silent maintenance / installer / uninstaller CLI flags before touching GUI/WebView2.
	if handled, code := eng.HandleMaintenanceCLI(args); handled {
		os.Exit(code)
	}

	startMode := "normal"
	startHidden := false
	winW, winH := 1040, 720
	minW, minH := 840, 560

	for _, a := range args {
		switch a {
		case "--tray":
			startMode = "tray"
			startHidden = true
		case "--post-install":
			startMode = "post-install"
			winW, winH = 460, 290
			minW, minH = 420, 260
		}
	}

	hooks := backend.NewPlatformHooks()
	app := NewApp(eng, hooks, args, startMode)

	err := wails.Run(&options.App{
		Title:             "目录密匣 — 跨平台轻量级目录密码保护工具",
		Width:             winW,
		Height:            winH,
		MinWidth:          minW,
		MinHeight:         minH,
		Frameless:         true,
		StartHidden:       startHidden,
		HideWindowOnClose: false,
		BackgroundColour:  &options.RGBA{R: 239, G: 244, B: 250, A: 255},
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		OnStartup:     app.startup,
		OnDomReady:    app.domReady,
		OnBeforeClose: app.beforeClose,
		OnShutdown:    app.shutdown,
		SingleInstanceLock: &options.SingleInstanceLock{
			UniqueId:               "com.yinchaow.dirlocker.single-instance",
			OnSecondInstanceLaunch: app.onSecondInstanceLaunch,
		},
		Bind: []interface{}{
			app,
		},
		DragAndDrop: &options.DragAndDrop{
			EnableFileDrop:     true,
			DisableWebViewDrop: false,
			CSSDropProperty:    "--wails-drop-target",
			CSSDropValue:       "drop",
		},
		Windows: &windows.Options{
			WebviewIsTransparent: false,
			WindowIsTranslucent:  false,
			Theme:                windows.SystemDefault,
		},
		Linux: &linux.Options{
			WindowIsTranslucent: false,
			WebviewGpuPolicy:    linux.WebviewGpuPolicyOnDemand,
			ProgramName:         "dirlocker",
		},
	})

	if err != nil {
		log.Fatal(err)
	}
}
