#cs ----------------------------------------------------------------------------

 AutoIt Version: 3.3.12.0
 Author:         TheHighFish

 Required software:
  * AutoIT
  * HTML Help Workshop
  * 7-zip

#ce ----------------------------------------------------------------------------

#include <Array.au3>
#include <StringConstants.au3>

Local $pre_created_release_dir  = "##_Hiss_Release_Directory_##"
Local $binary_dir               = "Release"
Local $binary_optimized_dir     = "Release - Optimized"
Local $openppl_dir              = "OpenPPL"
Local $openppl_library_dir      = $openppl_dir & "\OpenPPL_Library"
Local $release_notes            = $pre_created_release_dir & "\documents\Hiss Release Notes.txt"
Local $version_file              = "Hiss\OpenHoldem.rc"

WinMinimizeAll()
; Build manuals if needed
Local $choice = MsgBox(0x24, "build manuals?", "Build Hiss and OpenPPL manuals?")
If ($choice = 6) Then
   ShellExecuteWait("build_manual.bat", "", "OpenPPL\OpenPPL_manual\")
   ShellExecuteWait("build_manual.bat", "", "Documentation\")
EndIf
; Build RebuyDemo.exe
ShellExecuteWait("BuildRebuyDemo.bat", "", "RebuyDemo\")
MsgBox(0, "Next Step", "Have a look at http://code.google.com/p/openholdembot/source/list and ask the developers, if all work is completed and remind them to update the release-notes")
; Open release-notes for editing
ShellExecute($release_notes)
Sleep(2000)
MsgBox(0, "Next Step", "Update the version and release-date in the release-notes ")
WinWaitClose("Hiss Release Notes")
MsgBox(0, "Next Step", "Change the version of Hiss. (Search the Hiss project for the old one, e.g. ""4.2.5"" and ""4.25"", but don't auto-replace) Hiss\stdafx.h Hiss\OpenHoldem.rc")
MsgBox(0, "Next Step", "Choose the correct build option. Usually optimized for OH, but debug if there are some known problems left. Release for the rest (as end-users usually miss debug-DLLs). Then rebuild everything")
; One we know the version we can prepare the directories
Local $new_bot_logic_dir        = NewHissDir() & "\bot_logic"
Local $new_openppl_library_dir  = $new_bot_logic_dir & "\OpenPPL_Library"
Local $new_default_bot_dir      = $new_bot_logic_dir & "\DefaultBot"
Local $new_vmware_keyboard_dir  = NewHissDir() & "\Keyboard_DLL_VmWare_Unity_Mode"
Local $new_tools_dir            = NewHissDir() & "\tools"
DirRemove(NewHissDir(), 1)
DirCopy($pre_created_release_dir, NewHissDir())
; Copy Hiss to the new main directory plus its LIBs to support linking
CopyNeededFile($binary_optimized_dir, NewHissDir(), "Hiss.exe")
; Copy tools like ManualMode to the tools-directory
CopyNeededFile($binary_dir, $new_tools_dir, "ManualMode.exe")
CopyNeededFile($binary_dir, $new_tools_dir, "OHReplay.exe")
CopyNeededFile($binary_dir, $new_tools_dir, "OpenReplayShooter.exe")
CopyNeededFile($binary_dir, $new_tools_dir, "Vision.exe")
; Copy DLLs to the new directory  plus their LIBs to support linking
CopyNeededFile($binary_dir, NewHissDir(), "debug.dll")
CopyNeededFile($binary_dir, NewHissDir(), "files.dll")
CopyNeededFile($binary_dir, NewHissDir(), "GamestateValidation.dll")
CopyNeededFile($binary_dir, NewHissDir(), "globals.dll")
CopyNeededFile($binary_dir, NewHissDir(), "keyboard.dll")
CopyNeededFile($binary_dir, NewHissDir(), "mouse.dll")
CopyNeededFile($binary_dir, NewHissDir(), "pokertracker_query_definitions.dll")
CopyNeededFile($binary_dir, NewHissDir(), "preferences.dll")
CopyNeededFile($binary_dir, NewHissDir(), "user.dll")
CopyNeededFile($binary_dir, NewHissDir(), "string_functions.dll")
CopyNeededFile($binary_dir, NewHissDir(), "window_functions.dll")
; DLLs needed by Vision
CopyNeededFile($binary_dir, $new_tools_dir, "string_functions.dll")
CopyNeededFile($binary_dir, $new_tools_dir, "window_functions.dll")
; Add Keyboard_DLL_VmWare_Unity_Mode into separate directory
; CopyNeededFile($binary_dir, $new_vmware_keyboard_dir, "Keyboard_DLL_VmWare_Unity_Mode.dll")
; CopyNeededFile($binary_dir, $new_vmware_keyboard_dir, "Keyboard_DLL_VmWare_Unity_Mode.lib")
; Remove the *.exp and unnecessary *.pdb files.
FileDelete(NewHissDir() & "\*.exp")
FileDelete(NewHissDir() & "\*.pdb")
; Add the OpenPPL-library
CopyNeededFile($openppl_library_dir, $new_openppl_library_dir, "*.ohf")
; Set default-bot and OpenPPL-library as read-only.
; Some people were smart enough to edit it and then wondered why it did no longer work.
FileSetAttrib($new_openppl_library_dir, "+R", 1)
FileSetAttrib($new_default_bot_dir, "+R", 1)
; Remove replay-direcoty (if existent), logs and other private data
DirRemove(NewHissDir() & "\Replay")
FileDelete(NewHissDir() & "\logs\*.*")
; Move everything to the userts desktop
DirMove(NewHissDir(), @DesktopDir & "\" & NewHissDir())
CreateArchive()
; Release it
MsgBox(0, "Next Step", "The new direcory is at your desktop. A zip-archive has been created. Test ""everything"", at least briefly that OH ""works"".")
MsgBox(0, "Next Step", "Push everything to GitHub. Then tag the release on GitHub. Comment: ""Tagging Hiss <version> for release"". Then attach the zip-file to the latest release at GitHub")
; Open release-notes for announcement
MsgBox(0, "Next Step", "Announce the new download in ""Hiss Stickies"" and post the release-notes.")
ShellExecute($release_notes)

Func CopyNeededFile($source_dir, $destination_dir, $name)
   Local $source = $source_dir & "\" & $name
   Local $destination = $destination_dir & "\" & $name
   If Not FileCopy($source, $destination, 9) Then
	  MsgBox(0, "Error", "Can't copy: " & $source & " to " & $destination)
	  Exit(-1)
   EndIf
EndFunc

Func HissVersion()
   Local $version_data = FileReadToArray($version_file)
   Local $line_of_version = _ArraySearch($version_data, "ProductVersion", 0, UBound($version_data), 0, 1)
   Local $version = $version_data[$line_of_version]
   $version = StringRight($version, 9)
   $version = StringReplace($version, """", "")
   $version = StringReplace($version, ",", ".")
   $version = StringStripWS($version, $STR_STRIPALL)
   Return $version
EndFunc

Func NewHissDir()
   Return "Hiss_" & HissVersion()
EndFunc

Func CreateArchive()
   Local $command = @Programfilesdir & "\7-zip\7z.exe"
   Local $parameters = "a -r " & NewHissDir() & ".zip " & NewHissDir()
   ShellExecuteWait($command, $parameters, @DesktopDir)
EndFunc
