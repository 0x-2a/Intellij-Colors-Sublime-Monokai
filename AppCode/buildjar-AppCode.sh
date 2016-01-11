#!/bin/sh

mkdir colors
cp SublimeMonokai.xml colors
touch IntelliJ\ IDEA\ Global\ Settings

jar cfM SublimeMonoKai.jar IntelliJ\ IDEA\ Global\ Settings colors

rm -r colors
rm IntelliJ\ IDEA\ Global\ Settings