"""Offline Argos-model adapter, using CTranslate2 directly to avoid a resident NLP stack.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import os
import re
import sys
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "4")
sys.stdin.reconfigure(encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8")
root = Path(sys.argv[1]).resolve()
loaded = {}

def translate(text, direction):
    import ctranslate2
    import sentencepiece as spm
    if direction not in ("en_zh", "zh_en"):
        raise ValueError("不支持的翻译方向")
    if direction not in loaded:
        loaded.clear()
        folder = root / direction
        model = next(folder.rglob("model.bin")).parent
        tokenizer_path = next(folder.rglob("sentencepiece.model"))
        tokenizer = spm.SentencePieceProcessor(model_file=str(tokenizer_path))
        engine = ctranslate2.Translator(str(model), device="cpu", compute_type="int8", inter_threads=1, intra_threads=4)
        loaded[direction] = (engine, tokenizer)
    engine, tokenizer = loaded[direction]
    paragraphs = []
    for paragraph in text.split("\n"):
        if not paragraph.strip():
            paragraphs.append("")
            continue
        segments = re.split(r"(?<=[。！？])|(?<=[.!?])\s+", paragraph)
        tokens = []
        for segment in segments:
            encoded = tokenizer.encode(segment.strip(), out_type=str)
            # Bound model input without silently truncating a long sentence.
            tokens.extend(encoded[i:i + 240] for i in range(0, len(encoded), 240))
        predictions = engine.translate_batch(tokens, beam_size=3, max_input_length=256, max_decoding_length=512)
        joiner = " " if direction == "zh_en" else ""
        paragraphs.append(joiner.join(tokenizer.decode(p.hypotheses[0]).replace("▁", " ").strip() for p in predictions))
    return "\n".join(paragraphs)

for line in sys.stdin:
    request = {}
    try:
        request = json.loads(line)
        text = request.get("text", "")
        if not isinstance(text, str) or len(text) > 20000:
            raise ValueError("原文长度无效")
        result = {"id": request["id"], "text": translate(text, request["direction"])}
    except Exception as error:
        result = {"id": request.get("id", -1), "error": "离线翻译失败：" + str(error)}
    print(json.dumps(result, ensure_ascii=False), flush=True)
