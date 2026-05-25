#!/usr/bin/env bash
# _name_for.sh <app_dir_name>  -- print friendly display name.
# Override table first; fall back to spaced + capitalized dir name.
case "$1" in
    launcher)        echo "Homebrew" ;;
    wav_player)      echo "WAV Player" ;;
    font_test)       echo "Font Test" ;;
    multitouch_test) echo "Multitouch" ;;
    brightness_test) echo "Brightness" ;;
    button_test)     echo "Button Test" ;;
    fs_demo)         echo "FS Demo" ;;
    fs_gui)          echo "FS GUI" ;;
    fs_ls)           echo "FS List" ;;
    fs_read)         echo "FS Read" ;;
    fs_test)         echo "FS Test" ;;
    fs_write)        echo "FS Write" ;;
    api_tests)       echo "API Tests" ;;
    *)
        # default: underscore_separated -> Spaced + Title-Cased
        echo "$1" | awk '{
            n = split($0, a, "_");
            for (i = 1; i <= n; i++) {
                printf("%s%s%s",
                    (i > 1) ? " " : "",
                    toupper(substr(a[i], 1, 1)),
                    substr(a[i], 2));
            }
            print "";
        }'
        ;;
esac
