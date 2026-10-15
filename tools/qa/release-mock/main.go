// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

// A stand-in for GitHub's release list and release downloads, on loopback, for the update and
// content QA.
//
// It serves what the client reads and in the shape it reads it:
//
//	GET /repos/<owner>/<repo>/releases           the release list, built from -releases
//	GET /<owner>/<repo>/releases/download/<tag>/<asset>
//	                                             a 302 to /storage/..., as github.com answers
//	GET /<tag>/<asset>                           the same for DAR_QA_CONTENT_BASE, from -content
//	GET /storage/...                             the bytes, ranged requests answered with 206
//
// -releases holds one folder per release tag with its assets and an optional body.md; sizes and
// SHA-256 digests in the list are computed from the files. -content is a flat folder: only the
// last path segment picks the file, so any tag resolves to it.
//
// -norange, -drop-after and -throttle produce the failures a healthy connection never reaches;
// -expire-after makes every storage URL refuse (403) after that many requests, the way a signed
// GitHub storage URL expires, so a client has to go back through the redirect.
// Every request is logged to -log as one JSON line with its range and the bytes actually sent,
// which is how a test measures what a delta downloaded.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type asset struct {
	Name   string `json:"name"`
	Size   int64  `json:"size"`
	URL    string `json:"browser_download_url"`
	Digest string `json:"digest"`
}

type release struct {
	Tag        string  `json:"tag_name"`
	Name       string  `json:"name"`
	Body       string  `json:"body"`
	HTMLURL    string  `json:"html_url"`
	Draft      bool    `json:"draft"`
	Prerelease bool    `json:"prerelease"`
	Assets     []asset `json:"assets"`
}

type options struct {
	releases    string
	content     string
	noRange     bool
	dropAfter   int64
	throttle    int64
	redirect    bool
	expireAfter int64
}

var (
	// Each redirect hands out a storage URL of its own, counted separately.
	nonce   atomic.Int64
	storage sync.Map
)

// expired answers 403 once the storage URL of this request has been used up.
func expired(opts options, w http.ResponseWriter, r *http.Request) bool {
	if opts.expireAfter <= 0 {
		return false
	}
	counter, _ := storage.LoadOrStore(r.URL.RawQuery, new(atomic.Int64))
	if counter.(*atomic.Int64).Add(1) <= opts.expireAfter {
		return false
	}
	record(logEntry{Method: r.Method, Path: r.URL.Path, Range: r.Header.Get("Range"), Status: http.StatusForbidden})
	http.Error(w, "the signature has expired", http.StatusForbidden)
	return true
}

type logEntry struct {
	Time   string `json:"time"`
	Method string `json:"method"`
	Path   string `json:"path"`
	Range  string `json:"range,omitempty"`
	Status int    `json:"status"`
	Bytes  int64  `json:"bytes"`
}

var (
	logMutex sync.Mutex
	logFile  *os.File
)

func record(entry logEntry) {
	if logFile == nil {
		return
	}
	entry.Time = time.Now().UTC().Format(time.RFC3339Nano)
	line, _ := json.Marshal(entry)
	logMutex.Lock()
	defer logMutex.Unlock()
	logFile.Write(append(line, '\n'))
}

// countingWriter counts what reaches the client, throttles it and cuts it short on demand.
type countingWriter struct {
	http.ResponseWriter
	status  int
	written int64
	limit   int64
	rate    int64
	started time.Time
}

func (w *countingWriter) WriteHeader(status int) {
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func (w *countingWriter) Write(data []byte) (int, error) {
	if w.limit > 0 && w.written+int64(len(data)) > w.limit {
		data = data[:w.limit-w.written]
		n, _ := w.ResponseWriter.Write(data)
		w.written += int64(n)
		// Drop the connection mid-body, as a flaky line would.
		if hijacker, ok := w.ResponseWriter.(http.Hijacker); ok {
			if connection, _, err := hijacker.Hijack(); err == nil {
				connection.Close()
			}
		}
		return n, io.ErrClosedPipe
	}
	n, err := w.ResponseWriter.Write(data)
	w.written += int64(n)
	if w.rate > 0 {
		expected := time.Duration(float64(w.written) / float64(w.rate) * float64(time.Second))
		if elapsed := time.Since(w.started); elapsed < expected {
			time.Sleep(expected - elapsed)
		}
	}
	return n, err
}

func digestOf(path string) (string, int64, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer file.Close()
	hash := sha256.New()
	size, err := io.Copy(hash, file)
	if err != nil {
		return "", 0, err
	}
	return "sha256:" + hex.EncodeToString(hash.Sum(nil)), size, nil
}

func versionKey(tag string) []int {
	parts := strings.Split(tag, ".")
	key := make([]int, 3)
	for index := 0; index < len(parts) && index < 3; index++ {
		key[index], _ = strconv.Atoi(parts[index])
	}
	return key
}

func releaseList(opts options, host, owner, repo string) ([]release, error) {
	entries, err := os.ReadDir(opts.releases)
	if err != nil {
		return nil, err
	}
	var list []release
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		tag := entry.Name()
		folder := filepath.Join(opts.releases, tag)
		item := release{Tag: tag, Name: tag, HTMLURL: fmt.Sprintf("https://github.com/%s/%s/releases/tag/%s", owner, repo, tag)}
		if body, err := os.ReadFile(filepath.Join(folder, "body.md")); err == nil {
			item.Body = string(body)
		}
		files, err := os.ReadDir(folder)
		if err != nil {
			return nil, err
		}
		for _, file := range files {
			if file.IsDir() || file.Name() == "body.md" {
				continue
			}
			digest, size, err := digestOf(filepath.Join(folder, file.Name()))
			if err != nil {
				return nil, err
			}
			item.Assets = append(item.Assets, asset{
				Name:   file.Name(),
				Size:   size,
				URL:    fmt.Sprintf("http://%s/%s/%s/releases/download/%s/%s", host, owner, repo, tag, file.Name()),
				Digest: digest,
			})
		}
		list = append(list, item)
	}
	sort.Slice(list, func(a, b int) bool {
		left, right := versionKey(list[a].Tag), versionKey(list[b].Tag)
		for index := range left {
			if left[index] != right[index] {
				return left[index] > right[index]
			}
		}
		return list[a].Tag > list[b].Tag
	})
	return list, nil
}

// serveFile answers with the bytes of path, ranges and all, under the failure modes asked for.
func serveFile(opts options, w http.ResponseWriter, r *http.Request, path string) {
	writer := &countingWriter{ResponseWriter: w, status: http.StatusOK, limit: opts.dropAfter, rate: opts.throttle, started: time.Now()}
	rangeHeader := r.Header.Get("Range")
	defer func() {
		record(logEntry{Method: r.Method, Path: r.URL.Path, Range: rangeHeader, Status: writer.status, Bytes: writer.written})
	}()
	file, err := os.Open(path)
	if err != nil {
		writer.status = http.StatusNotFound
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		writer.status = http.StatusInternalServerError
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if opts.noRange {
		r.Header.Del("Range")
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeContent(writer, r, "", info.ModTime(), file)
}

func main() {
	var opts options
	address := flag.String("listen", "127.0.0.1:8791", "loopback address to listen on")
	logPath := flag.String("log", "", "JSON lines request log")
	flag.StringVar(&opts.releases, "releases", "", "folder of release folders named by tag")
	flag.StringVar(&opts.content, "content", "", "flat folder of content assets")
	flag.BoolVar(&opts.noRange, "norange", false, "answer every request with the whole file")
	flag.Int64Var(&opts.dropAfter, "drop-after", 0, "close the connection after this many body bytes")
	flag.Int64Var(&opts.throttle, "throttle", 0, "bytes per second per request, 0 for unlimited")
	flag.BoolVar(&opts.redirect, "redirect", true, "send release downloads through a storage redirect, as github.com does")
	flag.Int64Var(&opts.expireAfter, "expire-after", 0, "refuse a storage URL after this many requests, 0 for never")
	timeout := flag.Duration("timeout", 30*time.Minute, "exit after this long")
	flag.Parse()

	host, _, err := net.SplitHostPort(*address)
	if err != nil || (host != "127.0.0.1" && host != "localhost" && host != "::1") {
		log.Fatalf("listen on a loopback address only, not %q", *address)
	}
	if *logPath != "" {
		if logFile, err = os.OpenFile(*logPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644); err != nil {
			log.Fatal(err)
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /repos/{owner}/{repo}/releases", func(w http.ResponseWriter, r *http.Request) {
		list, err := releaseList(opts, r.Host, r.PathValue("owner"), r.PathValue("repo"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(list)
		record(logEntry{Method: r.Method, Path: r.URL.Path, Status: http.StatusOK})
	})
	mux.HandleFunc("GET /{owner}/{repo}/releases/download/{tag}/{asset}", func(w http.ResponseWriter, r *http.Request) {
		storage := fmt.Sprintf("/storage/releases/%s/%s?se=%d&n=%d&sig=mock", r.PathValue("tag"), r.PathValue("asset"), time.Now().Unix()+300, nonce.Add(1))
		if opts.redirect {
			record(logEntry{Method: r.Method, Path: r.URL.Path, Range: r.Header.Get("Range"), Status: http.StatusFound})
			http.Redirect(w, r, storage, http.StatusFound)
			return
		}
		serveFile(opts, w, r, filepath.Join(opts.releases, r.PathValue("tag"), r.PathValue("asset")))
	})
	mux.HandleFunc("GET /{tag}/{asset}", func(w http.ResponseWriter, r *http.Request) {
		storage := fmt.Sprintf("/storage/content/%s?se=%d&n=%d&sig=mock", r.PathValue("asset"), time.Now().Unix()+300, nonce.Add(1))
		if opts.redirect {
			record(logEntry{Method: r.Method, Path: r.URL.Path, Range: r.Header.Get("Range"), Status: http.StatusFound})
			http.Redirect(w, r, storage, http.StatusFound)
			return
		}
		serveFile(opts, w, r, filepath.Join(opts.content, r.PathValue("asset")))
	})
	mux.HandleFunc("GET /storage/releases/{tag}/{asset}", func(w http.ResponseWriter, r *http.Request) {
		if expired(opts, w, r) {
			return
		}
		serveFile(opts, w, r, filepath.Join(opts.releases, r.PathValue("tag"), r.PathValue("asset")))
	})
	mux.HandleFunc("GET /storage/content/{asset}", func(w http.ResponseWriter, r *http.Request) {
		if expired(opts, w, r) {
			return
		}
		serveFile(opts, w, r, filepath.Join(opts.content, r.PathValue("asset")))
	})

	listener, err := net.Listen("tcp", *address)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("release mock on http://%s/ (releases %q, content %q)\n", *address, opts.releases, opts.content)
	server := &http.Server{Handler: mux}
	go func() {
		time.Sleep(*timeout)
		server.Close()
	}()
	if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
