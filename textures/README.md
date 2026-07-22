# textures/

このディレクトリにテクスチャ画像（PNG/JPG等）を置く。
ビルド時に実行ファイルの隣の `textures/` へコピーされ、`Renderer` が
`"textures/<ファイル名>"` として読み込む（stb_image）。

Phase 8（テクスチャマッピング）で使用する画像をここに追加すること。
