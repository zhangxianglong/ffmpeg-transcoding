package main

import (
	"os"
	"os/signal"
	"syscall"
	"transcoding"
)

func CatchSignal() {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGUSR1)
	<-sig
	panic("")
}

func main() {
	go CatchSignal()
	tscp := transcoding.NewTransCoding()
	tscp.Init()
	ti := transcoding.TransInfo{
		Input_file_name: "transer.mp4",
		Output_path:     "/Users/zhangxianglong/resource",
		Thread_count:    9,
	}
	tscp.NewTransTask(ti)
}
