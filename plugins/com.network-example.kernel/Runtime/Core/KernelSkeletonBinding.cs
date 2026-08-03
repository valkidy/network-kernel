using System;
using UnityEngine;

namespace NetworkExample.Kernel
{
    [DisallowMultipleComponent]
    public sealed class KernelSkeletonBinding : MonoBehaviour
    {
        public const uint DefaultSkeletonAssetId = 1;
        public const int DefaultBoneCount = 41;
        public const ulong DefaultSkeletonContentHash =
            0x1c171165d9bb479bUL;

        private static readonly string[] DefaultBoneNames =
        {
            "SIM_Root",
            "JNT_BodyRoot",
            "LOC_FrontArc",
            "LOC_Com",
            "JNT_LegRearRight_Hip",
            "GEO_LegRearRight_Upper_Node",
            "JNT_LegRearRight_Knee",
            "GEO_LegRearRight_Blade_Node",
            "GEO_LegRearRight_Lower_Node",
            "JNT_LegRearRight_Foot",
            "JNT_LegFrontRight_Hip",
            "GEO_LegFrontRight_Upper_Node",
            "JNT_LegFrontRight_Knee",
            "GEO_LegFrontRight_Blade_Node",
            "GEO_LegFrontRight_Lower_Node",
            "JNT_LegFrontRight_Foot",
            "JNT_LegRearLeft_Hip",
            "GEO_LegRearLeft_Upper_Node",
            "JNT_LegRearLeft_Knee",
            "GEO_LegRearLeft_Blade_Node",
            "GEO_LegRearLeft_Lower_Node",
            "JNT_LegRearLeft_Foot",
            "JNT_LegFrontLeft_Hip",
            "GEO_LegFrontLeft_Upper_Node",
            "JNT_LegFrontLeft_Knee",
            "GEO_LegFrontLeft_Blade_Node",
            "GEO_LegFrontLeft_Lower_Node",
            "JNT_LegFrontLeft_Foot",
            "GEO_RootMovementBox_Node",
            "JNT_TailBase",
            "GEO_Tail_Node",
            "JNT_TailTip",
            "JNT_LowerThorax",
            "GEO_LowerThorax_Node",
            "JNT_Head",
            "LOC_Mouth",
            "GEO_Head_Node",
            "JNT_DorsalSpine",
            "GEO_DorsalSpine_Node",
            "GEO_BodyCore_Node",
            "SKIN_BindCarrier",
        };

        private static readonly int[] DefaultParentIndices =
        {
            -1, 0, 1, 1, 1, 4, 4, 6, 6, 6,
            1, 10, 10, 12, 12, 12, 1, 16, 16, 18,
            18, 18, 1, 22, 22, 24, 24, 24, 1, 1,
            29, 29, 1, 32, 1, 34, 34, 1, 37, 1,
            0,
        };

        [Tooltip("Native skeleton asset ID. Defaults to simplified_monster_sim_v4.")]
        public uint SkeletonAssetId = DefaultSkeletonAssetId;

        [Tooltip("Native skeleton content hash. Defaults to simplified_monster_sim_v4.")]
        public ulong SkeletonContentHash = DefaultSkeletonContentHash;

        [Tooltip("Optional hierarchy root. When empty, this component scans its GameObject.")]
        public Transform SkeletonRoot;

        [Tooltip("Automatically map known skeleton bones by unique FBX transform name.")]
        public bool AutoMapKnownSkeleton = true;

        [Tooltip(
            "Apply Native pose deltas on top of the imported FBX bind pose. " +
            "Enabled by Reset when this component is attached in the Unity Editor.")]
        public bool PreservePrefabBindPose;

        public Transform[] Bones = Array.Empty<Transform>();

        public static string GetDefaultBoneName(int boneIndex)
        {
            if (boneIndex < 0 || boneIndex >= DefaultBoneNames.Length)
            {
                throw new ArgumentOutOfRangeException(nameof(boneIndex));
            }
            return DefaultBoneNames[boneIndex];
        }

        public static int GetDefaultParentBoneIndex(int boneIndex)
        {
            if (boneIndex < 0 || boneIndex >= DefaultParentIndices.Length)
            {
                throw new ArgumentOutOfRangeException(nameof(boneIndex));
            }
            return DefaultParentIndices[boneIndex];
        }

        public bool TryAutoMap(out string error)
        {
            if (!IsDefaultSkeleton)
            {
                error =
                    $"No automatic bone profile for skeleton asset={SkeletonAssetId} " +
                    $"hash=0x{SkeletonContentHash:x16}.";
                return false;
            }

            Transform searchRoot = SkeletonRoot != null ? SkeletonRoot : transform;
            Transform[] descendants =
                searchRoot.GetComponentsInChildren<Transform>(true);
            var mappedBones = new Transform[DefaultBoneNames.Length];
            for (int transformIndex = 0;
                 transformIndex < descendants.Length;
                 ++transformIndex)
            {
                Transform candidate = descendants[transformIndex];
                string candidateName = StripNamespace(candidate.name);
                int boneIndex = FindDefaultBoneIndex(candidateName);
                if (boneIndex < 0)
                {
                    continue;
                }
                if (mappedBones[boneIndex] != null)
                {
                    error =
                        $"Bone name '{DefaultBoneNames[boneIndex]}' is not unique " +
                        $"under '{searchRoot.name}'.";
                    return false;
                }
                mappedBones[boneIndex] = candidate;
            }

            for (int boneIndex = 0; boneIndex < mappedBones.Length; ++boneIndex)
            {
                if (mappedBones[boneIndex] == null)
                {
                    error =
                        $"Missing bone '{DefaultBoneNames[boneIndex]}' " +
                        $"under '{searchRoot.name}'. Disable Optimize Game Objects and " +
                        "preserve the complete FBX hierarchy.";
                    return false;
                }

                int parentIndex = DefaultParentIndices[boneIndex];
                if (parentIndex >= 0 &&
                    mappedBones[boneIndex].parent != mappedBones[parentIndex])
                {
                    error =
                        $"Bone '{DefaultBoneNames[boneIndex]}' must be a direct child " +
                        $"of '{DefaultBoneNames[parentIndex]}'.";
                    return false;
                }
            }

            Bones = mappedBones;
            error = null;
            return true;
        }

        [ContextMenu("Auto Map Known Skeleton Bones")]
        private void AutoMapKnownSkeletonBones()
        {
            if (!TryAutoMap(out string error))
            {
                Debug.LogError(error, this);
            }
        }

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
            if (AutoMapKnownSkeleton && !HasCompleteBoneArray())
            {
                if (!TryAutoMap(out error))
                {
                    return false;
                }
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
            if (IsDefaultSkeleton)
            {
                if (Bones.Length != DefaultBoneNames.Length)
                {
                    error =
                        $"simplified_monster_sim_v4 requires " +
                        $"{DefaultBoneNames.Length} bones, found {Bones.Length}.";
                    return false;
                }
                for (int index = 0; index < Bones.Length; ++index)
                {
                    if (StripNamespace(Bones[index].name) !=
                        DefaultBoneNames[index])
                    {
                        error =
                            $"Bones[{index}] must map to " +
                            $"'{DefaultBoneNames[index]}', found '{Bones[index].name}'.";
                        return false;
                    }
                    int parentIndex = DefaultParentIndices[index];
                    if (parentIndex >= 0 &&
                        Bones[index].parent != Bones[parentIndex])
                    {
                        error =
                            $"Bones[{index}] has an unexpected parent for " +
                            $"'{DefaultBoneNames[index]}'.";
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

        private bool IsDefaultSkeleton =>
            SkeletonAssetId == DefaultSkeletonAssetId &&
            SkeletonContentHash == DefaultSkeletonContentHash;

        private bool HasCompleteBoneArray()
        {
            if (Bones == null || Bones.Length == 0)
            {
                return false;
            }
            for (int index = 0; index < Bones.Length; ++index)
            {
                if (Bones[index] == null)
                {
                    return false;
                }
            }
            return true;
        }

        private static int FindDefaultBoneIndex(string boneName)
        {
            for (int index = 0; index < DefaultBoneNames.Length; ++index)
            {
                if (DefaultBoneNames[index] == boneName)
                {
                    return index;
                }
            }
            return -1;
        }

        private static string StripNamespace(string value)
        {
            int colon = value.LastIndexOf(':');
            int pipe = value.LastIndexOf('|');
            int separator = Math.Max(colon, pipe);
            return separator >= 0 ? value.Substring(separator + 1) : value;
        }

        private void Reset()
        {
            SkeletonAssetId = DefaultSkeletonAssetId;
            SkeletonContentHash = DefaultSkeletonContentHash;
            AutoMapKnownSkeleton = true;
            PreservePrefabBindPose = true;
            TryAutoMap(out string _);
        }

#if UNITY_EDITOR
        private void OnValidate()
        {
            if (AutoMapKnownSkeleton && IsDefaultSkeleton)
            {
                TryAutoMap(out string _);
            }
        }
#endif
    }
}
