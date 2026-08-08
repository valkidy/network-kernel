using System;
using UnityEngine;

namespace NetworkExample.Kernel
{
    /// <summary>
    /// Maps a skeleton's Native bone indices onto Unity transforms.
    /// </summary>
    /// <remarks>
    /// The bone layout comes from the skeleton manifest in the gameplay catalog
    /// bundle (see <see cref="KernelSkeletonManifestCatalog"/>), not from a
    /// table compiled into this package. That is what lets a rig be changed, or
    /// a new one added, without rebuilding the Unity package: the client reads
    /// the same manifest the server loaded, which in client mode arrives over
    /// the wire.
    ///
    /// Load the catalog before this component validates or auto-maps. With no
    /// manifest loaded there is nothing to map against, and the component says
    /// so rather than falling back to a guess.
    /// </remarks>
    [DisallowMultipleComponent]
    public sealed class KernelSkeletonBinding : MonoBehaviour
    {
        [Tooltip("Native skeleton asset ID, as authored in the gameplay catalog.")]
        public uint SkeletonAssetId = 1;

        [Tooltip(
            "Native skeleton content hash. Left at 0 this is resolved from the " +
            "manifest; set non-zero it must match, which catches a scene still " +
            "holding a rig that has since been rebuilt.")]
        public ulong SkeletonContentHash;

        [Tooltip("Optional hierarchy root. When empty, this component scans its GameObject.")]
        public Transform SkeletonRoot;

        [Tooltip("Automatically map manifest bones by unique transform name.")]
        public bool AutoMapKnownSkeleton = true;

        [Tooltip(
            "Apply Native pose deltas on top of the imported bind pose. " +
            "Enabled by Reset when this component is attached in the Unity Editor.")]
        public bool PreservePrefabBindPose;

        public Transform[] Bones = Array.Empty<Transform>();

        /// <summary>
        /// The manifest this binding resolves to, or null when the gameplay
        /// catalog has not been loaded or does not carry this asset.
        /// </summary>
        public KernelSkeletonManifest Manifest
        {
            get
            {
                KernelSkeletonManifestCatalog.TryGet(
                    SkeletonAssetId,
                    SkeletonContentHash,
                    out KernelSkeletonManifest manifest);
                return manifest;
            }
        }

        public bool TryGetManifest(out KernelSkeletonManifest manifest, out string error)
        {
            if (!KernelSkeletonManifestCatalog.TryGet(
                    SkeletonAssetId,
                    SkeletonContentHash,
                    out manifest))
            {
                error = KernelSkeletonManifestCatalog.Manifests.Count == 0
                    ? "No skeleton manifests are loaded. Load the gameplay catalog " +
                      "bundle with KernelSkeletonManifestCatalog.TryLoadFromBundle " +
                      "before binding a skeleton."
                    : $"No manifest for skeleton asset={SkeletonAssetId} " +
                      $"hash=0x{SkeletonContentHash:x16}.";
                return false;
            }
            error = null;
            return true;
        }

        public bool TryAutoMap(out string error)
        {
            if (!TryGetManifest(out KernelSkeletonManifest manifest, out error))
            {
                return false;
            }

            Transform searchRoot = SkeletonRoot != null ? SkeletonRoot : transform;
            Transform[] descendants =
                searchRoot.GetComponentsInChildren<Transform>(true);
            var mappedBones = new Transform[manifest.BoneCount];
            for (int transformIndex = 0;
                 transformIndex < descendants.Length;
                 ++transformIndex)
            {
                Transform candidate = descendants[transformIndex];
                int boneIndex = manifest.IndexOf(StripNamespace(candidate.name));
                if (boneIndex < 0)
                {
                    continue;
                }
                if (mappedBones[boneIndex] != null)
                {
                    error =
                        $"Bone name '{manifest.Bones[boneIndex].Name}' is not unique " +
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
                        $"Missing bone '{manifest.Bones[boneIndex].Name}' " +
                        $"under '{searchRoot.name}'. Disable Optimize Game Objects and " +
                        "preserve the complete imported hierarchy.";
                    return false;
                }

                int parentIndex = manifest.Bones[boneIndex].ParentIndex;
                if (parentIndex >= 0 &&
                    mappedBones[boneIndex].parent != mappedBones[parentIndex])
                {
                    error =
                        $"Bone '{manifest.Bones[boneIndex].Name}' must be a direct child " +
                        $"of '{manifest.Bones[parentIndex].Name}'.";
                    return false;
                }
            }

            Bones = mappedBones;
            SkeletonContentHash = manifest.ContentHash;
            error = null;
            return true;
        }

        [ContextMenu("Auto Map Skeleton Bones From Manifest")]
        private void AutoMapSkeletonBones()
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
            if (!TryGetManifest(out KernelSkeletonManifest manifest, out error))
            {
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
            if (Bones.Length != manifest.BoneCount)
            {
                error =
                    $"{manifest.Name} requires {manifest.BoneCount} bones, " +
                    $"found {Bones.Length}.";
                return false;
            }
            for (int index = 0; index < Bones.Length; ++index)
            {
                if (StripNamespace(Bones[index].name) != manifest.Bones[index].Name)
                {
                    error =
                        $"Bones[{index}] must map to " +
                        $"'{manifest.Bones[index].Name}', found '{Bones[index].name}'.";
                    return false;
                }
                int parentIndex = manifest.Bones[index].ParentIndex;
                if (parentIndex >= 0 && Bones[index].parent != Bones[parentIndex])
                {
                    error =
                        $"Bones[{index}] has an unexpected parent for " +
                        $"'{manifest.Bones[index].Name}'.";
                    return false;
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

        /// <summary>
        /// Drops an importer's namespace or path prefix, so "rig:JNT_Body" and
        /// "Armature|JNT_Body" both match the manifest's "JNT_Body".
        /// </summary>
        private static string StripNamespace(string value)
        {
            int colon = value.LastIndexOf(':');
            int pipe = value.LastIndexOf('|');
            int separator = Math.Max(colon, pipe);
            return separator >= 0 ? value.Substring(separator + 1) : value;
        }

        private void Reset()
        {
            AutoMapKnownSkeleton = true;
            PreservePrefabBindPose = true;
            TryAutoMap(out string _);
        }

#if UNITY_EDITOR
        private void OnValidate()
        {
            if (AutoMapKnownSkeleton && KernelSkeletonManifestCatalog.Manifests.Count > 0)
            {
                TryAutoMap(out string _);
            }
        }
#endif
    }
}
