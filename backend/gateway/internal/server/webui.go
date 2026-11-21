package server

import (
	"embed"
	"io/fs"
	stdhttp "net/http"
)

//go:embed webui/*
var webUIFiles embed.FS

// NewWebUIHandler serves the embedded Gateway management & live WebSocket debug console under `/ui/`.
func NewWebUIHandler() stdhttp.Handler {
	sub, err := fs.Sub(webUIFiles, "webui")
	if err != nil {
		return stdhttp.NotFoundHandler()
	}
	return stdhttp.StripPrefix("/ui/", stdhttp.FileServer(stdhttp.FS(sub)))
}
