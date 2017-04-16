#!/bin/sh
TARGET=eiz@10.1.0.10

ssh $TARGET msiexec /u SynchronousAudioRouter.msi
scp SarInstaller/bin/x64/Release/SynchronousAudioRouter.msi $TARGET:.
ssh $TARGET msiexec '/l*v' install.log /i SynchronousAudioRouter.msi

