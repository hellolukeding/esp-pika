package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"esp-pika/backend/voice-server/internal/speaker"
)

func main() {
	listenAddr := flag.String("listen", ":8081", "voice-server HTTP/WS listen address")
	qdrantURL := flag.String("qdrant", "http://127.0.0.1:6333", "Qdrant HTTP REST base URL")
	collection := flag.String("collection", "speaker_embeddings", "Qdrant 192-D collection name")
	threshold := flag.Float64("threshold", 0.6, "cosine identification threshold")
	flag.Parse()

	store := speaker.NewQdrantStore(*qdrantURL, *collection)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	if err := store.EnsureCollection(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "warning: qdrant collection init (%s): %v (fallback active)\n", *qdrantURL, err)
	}
	cancel()

	srv := &http.Server{
		Addr:    *listenAddr,
		Handler: speaker.NewServer(&speaker.SpectralEmbedder{}, store, float32(*threshold)).Handler(),
	}

	sigCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		<-sigCtx.Done()
		shutCtx, shutCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer shutCancel()
		_ = srv.Shutdown(shutCtx)
	}()

	fmt.Printf("voice-server listening on %s (qdrant=%s collection=%s)\n", *listenAddr, *qdrantURL, *collection)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		fmt.Fprintf(os.Stderr, "serve error: %v\n", err)
		os.Exit(1)
	}
}
