// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package osmisc

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// CopyFile copies a file to a given destination.
func CopyFile(src, dest string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	info, err := in.Stat()
	if err != nil {
		return err
	}
	out, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE, info.Mode().Perm())
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

// FileIsOpen returns whether the given file is open or not.
func FileIsOpen(f *os.File) bool {
	return f.Fd() != ^uintptr(0)
}

// CreateFile creates the file specified by the given path and all parent directories if they don't exist.
func CreateFile(path string) (*os.File, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0777); err != nil {
		return nil, fmt.Errorf("failed to make parent dirs of %s: %v", path, err)
	}
	return os.Create(path)
}
