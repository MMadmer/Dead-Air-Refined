#!/bin/bash
# all scene bags: model + bake + game files + preview
B="C:/Program Files (x86)/Steam/steamapps/common/Blender/blender.exe"
cd "D:/Games/Dead-Air-AssetCompare/work/backpack"
for v in ${@:-default kit_hunt backpack_light backpack_heavy backpack_travel backpack_stalker}; do
  echo "== $v"
  "$B" -b hull.blend --python bp_build.py -- $v 2>&1 | grep -E "BUILD|EXPORT|Error|Traceback|line [0-9]"
  PYTHONIOENCODING=utf-8 python bp_textures.py $v 2>&1 | tail -1
  "$B" -b --python bp_preview.py -- $v 0 2>&1 | grep -E "PREVIEW_DONE|Error|Traceback"
done
echo ALL_DONE
