using System;
using UnityEngine;

namespace NetworkExample.Kernel
{
    [DisallowMultipleComponent]
    public sealed class KernelSkeletonBinding : MonoBehaviour
    {
        public uint SkeletonAssetId;
        public ulong SkeletonContentHash;
        public Transform[] Bones = Array.Empty<Transform>();

        public bool TryValidate(out string error)
        {
            if (SkeletonAssetId == 0)
            {
                error = "SkeletonAssetId must be non-zero.";
                return false;
            }
            if (SkeletonContentHash == 0)
            {
                error = "SkeletonContentHash must be non-zero.";
                return false;
            }
            if (Bones == null || Bones.Length == 0)
            {
                error = "Bones must contain at least one transform.";
                return false;
            }
            for (int index = 0; index < Bones.Length; ++index)
            {
                if (Bones[index] == null)
                {
                    error = $"Bones[{index}] is null.";
                    return false;
                }
                for (int previous = 0; previous < index; ++previous)
                {
                    if (Bones[index] == Bones[previous])
                    {
                        error = $"Bones[{index}] duplicates Bones[{previous}].";
                        return false;
                    }
                }
            }

            error = null;
            return true;
        }

        public bool TryValidate(
            KernelSkeletonRenderState state,
            out string error)
        {
            if (!TryValidate(out error))
            {
                return false;
            }
            if (state.skeleton_asset_id != SkeletonAssetId)
            {
                error =
                    $"Skeleton asset mismatch: state={state.skeleton_asset_id}, binding={SkeletonAssetId}.";
                return false;
            }
            if (state.skeleton_content_hash != SkeletonContentHash)
            {
                error =
                    $"Skeleton content hash mismatch: state=0x{state.skeleton_content_hash:x16}, binding=0x{SkeletonContentHash:x16}.";
                return false;
            }
            if (state.bone_count != (uint)Bones.Length)
            {
                error =
                    $"Bone count mismatch: state={state.bone_count}, binding={Bones.Length}.";
                return false;
            }

            error = null;
            return true;
        }
    }
}
