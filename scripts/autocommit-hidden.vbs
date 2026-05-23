Option Explicit

Dim shell
Dim repoPath
Dim scriptPath
Dim command

repoPath = "C:\www\openholdembot_old"
scriptPath = repoPath & "\scripts\autocommit.ps1"

Set shell = CreateObject("WScript.Shell")
command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File " _
    & Chr(34) & scriptPath & Chr(34) & " -RepoPath " & Chr(34) & repoPath & Chr(34)

shell.Run command, 0, False
