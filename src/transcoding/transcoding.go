package transcoding

import (
	"log"
	"os"
	"strings"
)

/*
#cgo LDFLAGS:-L/usr/local/lib -lavformat -lavcodec -lavfilter -lswresample -lavutil -lpthread -lm -ldl
#cgo CFLAGS:-I/usr/local/include
#include "trans.h"
*/
import "C"

func init_ffmpeg() error {
	C.init_ffmpeg()
	return nil
}

type TransInfo struct {
	Input_file_name string
	Output_path     string
	Thread_count    int
}

type TransCoding struct {
}

func NewTransCoding() *TransCoding {
	p := &TransCoding{}
	return p
}

func (self *TransCoding) Init() {
	init_ffmpeg()
}

func (self *TransCoding) NewTransTask(info TransInfo) (err error) {
	if _, err = os.Stat(info.Input_file_name); err != nil {
		log.Println("input file is not exit")
		return err
	}

	var outputpath string
	if _, err = os.Stat(info.Output_path); err != nil {
		log.Println("output path is not exit ;create path:", info.Output_path)
		err = os.MkdirAll(info.Output_path, 0777)
		if err != nil {
			log.Println("create output path error:", err)
			return err
		}
	}
	if strings.HasSuffix(info.Output_path, "/") {
		index := strings.LastIndex(info.Output_path, "/")
		outputpath = info.Output_path[:index]
	} else {
		outputpath = info.Output_path
	}
	log.Println(info.Input_file_name)
	C.CreateTransTask((C.CString)(info.Input_file_name), (C.CString)(outputpath))
	return nil
}
