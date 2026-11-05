# Android / Termux

Android向け配布物は `pixd`、`pixctl`、`pix-ts` のCLIです。Androidアプリではありません。USBの許可取得にはTermux:APIアプリとTermux内の`termux-api`パッケージが必要です。

アーカイブを展開してそのディレクトリへ移動し、`termux-usb -l`で対象のUSBデバイスパスを確認します。PIX-DT181-PU0とPIX-DT295のどちらも同じ手順で、Termux:APIの許可を受けたFDから機種を判定します。

```sh
termux-usb -r -e './packaging/pix-termux --firmware ./firmware/dt181.bin --instance dt181 --system isdb-t --receiver 0 --channel T27 --time 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

`/dev/bus/usb/001/002`は`termux-usb -l`に表示された対象のパスへ置き換えます。`termux-usb`が許可済みFDをlauncher末尾の引数として渡し、launcherが現行3コマンド経路を次の順に実行します。

1. `pixd --fd N --firmware FILE --instance NAME`でUSBを所有するdaemonを起動します。通常Linux/macOSの`--device`起動も、`pixd --device PATH --firmware FILE`で使います。
2. `pixctl status`で同じdaemonのready状態を確認します（`--instance NAME`を付与）。
3. `pix-ts`で受信し、指定されたファイルへMPEG-TSを書き出します（`--instance NAME`を付与）。

launcherは作業用runtime directoryとdaemonを終了時に片付けます。`--firmware FILE`および`--instance <instance-name>`は必須です。`--instance`には呼出元が任意のインスタンス名を指定し、launcherはその値を`pixd`、`pixctl status`、`pix-ts`へ渡します（`--instance`の未指定・値欠落・空値は終了コード 2）。`--firmware`には接続機種に合う `firmware/dt181.bin` または `firmware/dt295.bin` を指定します。空白やUnicodeを含むパスも1つの引数として渡せます。`--runtime-dir`および`--group`はlauncherが管理するため指定しません。`pix-ts`では必須の`--receiver 0`と、`--system`、`--channel`または`--freq`、必要に応じて`--tsid`等で受信条件を指定します。短い試行なら`--time 15`を指定できます。

この経路は合成USB fixtureとAndroid 3 ABIのcross-build/packageで検証します。Termux:APIの権限prompt、接続機器、Android端末上の実受信・OS固有挙動は別途実機確認が必要です。
