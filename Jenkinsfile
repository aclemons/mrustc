#!/usr/bin/env groovy

// Copyright 2019 Andrew Clemons, Wellington New Zealand
// All rights reserved.
//
// Redlstribution and use of this script, with or without modification, is
// permitted provided that the following conditions are met:
//
// 1. Redistributions of this script must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
//  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
//  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
//  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
//  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
//  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

node {
    checkout scm

    ansiColor('xterm') {
        def image = docker.build("mrustc-base:${env.BUILD_ID}")

        image.inside {
            sh 'make RUSTCSRC'
            sh 'RUSTC_TARGET=x86_64-unknown-linux-gnu LLVM_TARGETS=x86 CC=gcc make -f minicargo.mk'
            sh 'RUSTC_TARGET=x86_64-unknown-linux-gnu LLVM_TARGETS=x86 CC=gcc make -j1 -C run_rustc'
        }
    }
}
