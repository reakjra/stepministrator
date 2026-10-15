# stepministrator

starts Arknights: Endfield without its launcher with wine

the game refuses to run unless its parent process is the launcher's signed (or whatever) `Games.exe`. this downloads only `Games.exe` from gryphline's own CDN, starts it suspended so it never runs and starts the game with it registered as the parent process

`Games.exe` is cached next to `stepministrator.exe` with a `Games.version` file and downloaded again only when the launcher version changes. If the version check fails for any reason it'll be used the cached one ofc

## Usage

it must run as a windows process in the same prefix as the game:

```sh
stepministrator.exe --api <url> --appcode <code> --channel <n> --sub-channel <n> --game <exe> [-- <game args>...]
```

e.g. Endfield global:

```sh
stepministrator.exe --api https://launcher.gryphline.com/api --appcode TiaytKBUIEdoEwRT --channel 6 --sub-channel 6 --game /path/to/Endfield.exe -- -launcher_lang=en-us -launcher_sub_channel=6
```

## Building

needs a mingw-w64 cross compiler:

```sh
make
```

[vendor/puff.c](https://github.com/madler/zlib/blob/v1.3.1/contrib/puff/puff.c)

## License

GPL-3.0.
