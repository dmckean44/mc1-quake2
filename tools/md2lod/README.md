# md2lod

Builds reduced-polygon MD2 models from your own Quake II pak files (the
models are id Software's, so generate them locally rather than shipping them).

    # replace models outright, keeping view weapons at 75%
    python3 md2lod.py baseq2/pak0.pak -o baseq2/lowpoly.pak -r 0.5 --rule 'models/weapons/v_*=0.75'

    # distant-model LODs, used when 64 world units cover fewer than sw_lodsize pixels
    python3 md2lod.py baseq2/pak0.pak -o baseq2/lod.pak -r 0.3 --suffix _lod \
        --min-normal-dot -0.5 --uv-tolerance 32 --rule 'models/weapons/v_*=1'

    # compare a model before/after
    python3 preview.py baseq2/pak0.pak baseq2/lowpoly.pak models/monsters/soldier/tris.md2 -o soldier.png

Needs Python 3 and numpy.
