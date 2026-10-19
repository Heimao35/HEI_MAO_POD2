"""Patch pinned IDF5.5.4 JPEG partial-constructor cleanup in the build tree only.

rx/tx/header allocation can fail before acquiring codec_base. Releasing NULL
must not decrement the shared codec reference owned by a cover decoder.
Reject changed anchors on SDK upgrade; installed and managed files stay intact.
"""
from pathlib import Path
import sys

def patched(text):
    old = '        ESP_RETURN_ON_ERROR(jpeg_release_codec_handle(decoder_engine->codec_base), TAG, "release codec failed");'
    if text.count(old) != 1:
        raise ValueError("JPEG cleanup: SDK anchor changed; review pinned driver")
    return text.replace(old, """        /* Player fix: constructor can fail before codec acquisition. A NULL
         * codec_base owns no reference; do not touch another decoder's count. */
        if (decoder_engine->codec_base) {
    """ + old + """
        }""")

def extract_delete(text):
    start = text.index('esp_err_t jpeg_del_decoder_engine(')
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'

def write_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')

if __name__ == '__main__':
    source, output = map(Path, sys.argv[1:3])
    result = patched(source.read_text(encoding='utf-8'))
    write_changed(output, result)
    if len(sys.argv) == 4:
        write_changed(Path(sys.argv[3]), extract_delete(result))
