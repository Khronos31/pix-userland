# Android / Termux

Android向け配布物は `pixd`、`pixctl`、`pix-ts` のCLIです。Androidアプリではありません。USBの許可取得にはTermux:APIアプリとTermux内の`termux-api`パッケージが必要です。

アーカイブを展開してそのディレクトリへ移動し、`termux-usb -l`で対象のUSBデバイスパスを確認します。PIX-DT181-PU0とPIX-DT295のどちらも同じ手順で、Termux:APIの許可を受けたFDから機種を判定します。

```sh
termux-usb -r -e './packaging/pix-termux --system isdb-t --channel T27 --time 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

`/dev/bus/usb/001/002`は`termux-usb -l`に表示された対象のパスへ置き換えます。`termux-usb`が許可済みFDをlauncher末尾の引数として渡し、launcherが現行3コマンド経路を次の順に実行します。

1. `pixd --fd N`でUSBを所有するdaemonを起動します。通常Linux/macOSの`--device`起動はそのまま利用できます。
2. `pixctl status`で同じdaemonのready状態を確認します。
3. `pix-ts`で受信し、指定されたファイルへMPEG-TSを書き出します。

launcherは作業用runtime directoryとdaemonを終了時に片付けます。`--instance`、`--runtime-dir`、`--group`はlauncherが管理するため指定しません。`pix-ts`の受信条件は`--system`、`--channel`または`--freq`、必要に応じて`--tsid`等で指定します。短い試行なら`--time 15`を指定できます。

この経路は合成USB fixtureとAndroid 3 ABIのcross-build/packageで検証します。Termux:APIの権限prompt、接続機器、Android端末上の実受信・OS固有挙動は別途実機確認が必要です。
