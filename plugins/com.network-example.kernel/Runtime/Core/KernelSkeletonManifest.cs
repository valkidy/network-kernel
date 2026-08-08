using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace NetworkExample.Kernel
{
    /// <summary>
    /// One bone of a skeleton manifest, in the order the Native kernel indexes
    /// bones. Rest values are the ozz rest pose, copied component for component
    /// out of the source asset -- nothing in the pipeline converts coordinates,
    /// so these are the same numbers the kernel solves against.
    /// </summary>
    public struct KernelSkeletonManifestBone
    {
        public string Name;
        public int ParentIndex;
        public float RestPositionX;
        public float RestPositionY;
        public float RestPositionZ;
        public float RestRotationX;
        public float RestRotationY;
        public float RestRotationZ;
        public float RestRotationW;
        public float RestScaleX;
        public float RestScaleY;
        public float RestScaleZ;
    }

    /// <summary>
    /// A skeleton's bone layout and bind pose, as baked next to its runtime
    /// asset. This is the only place the client can learn bone names: the C ABI
    /// carries an asset id, a content hash and a bone count, but no names.
    /// </summary>
    public sealed class KernelSkeletonManifest
    {
        public uint AssetId { get; internal set; }
        public ulong ContentHash { get; internal set; }
        public string Name { get; internal set; }
        public uint ManifestVersion { get; internal set; }
        public KernelSkeletonManifestBone[] Bones { get; internal set; }

        /// <summary>Rest poses are only present from manifest_version 2.</summary>
        public bool HasRestPose => ManifestVersion >= 2u;

        public int BoneCount => Bones == null ? 0 : Bones.Length;

        /// <summary>
        /// Model-space rest position of every bone, as x,y,z triples indexed by
        /// bone index.
        /// </summary>
        /// <remarks>
        /// Composed exactly the way a Transform hierarchy built from this
        /// manifest composes -- a parent's scale applies to its child's local
        /// position -- so this is what the rebuilt rig will actually measure,
        /// and it is checkable without Unity.
        /// </remarks>
        public float[] ComputeRestModelPositions()
        {
            int count = BoneCount;
            var positions = new float[count * 3];
            var rotations = new float[count * 4];
            var scales = new float[count * 3];
            for (int index = 0; index < count; ++index)
            {
                KernelSkeletonManifestBone bone = Bones[index];
                int parent = bone.ParentIndex;
                if (parent < 0)
                {
                    positions[index * 3 + 0] = bone.RestPositionX;
                    positions[index * 3 + 1] = bone.RestPositionY;
                    positions[index * 3 + 2] = bone.RestPositionZ;
                    rotations[index * 4 + 0] = bone.RestRotationX;
                    rotations[index * 4 + 1] = bone.RestRotationY;
                    rotations[index * 4 + 2] = bone.RestRotationZ;
                    rotations[index * 4 + 3] = bone.RestRotationW;
                    scales[index * 3 + 0] = bone.RestScaleX;
                    scales[index * 3 + 1] = bone.RestScaleY;
                    scales[index * 3 + 2] = bone.RestScaleZ;
                    continue;
                }

                float localX = bone.RestPositionX * scales[parent * 3 + 0];
                float localY = bone.RestPositionY * scales[parent * 3 + 1];
                float localZ = bone.RestPositionZ * scales[parent * 3 + 2];
                RotateByQuaternion(
                    rotations, parent, localX, localY, localZ,
                    out float rotatedX, out float rotatedY, out float rotatedZ);
                positions[index * 3 + 0] = positions[parent * 3 + 0] + rotatedX;
                positions[index * 3 + 1] = positions[parent * 3 + 1] + rotatedY;
                positions[index * 3 + 2] = positions[parent * 3 + 2] + rotatedZ;

                MultiplyQuaternions(
                    rotations, parent,
                    bone.RestRotationX, bone.RestRotationY,
                    bone.RestRotationZ, bone.RestRotationW,
                    rotations, index);
                scales[index * 3 + 0] = scales[parent * 3 + 0] * bone.RestScaleX;
                scales[index * 3 + 1] = scales[parent * 3 + 1] * bone.RestScaleY;
                scales[index * 3 + 2] = scales[parent * 3 + 2] * bone.RestScaleZ;
            }
            return positions;
        }

        private static void RotateByQuaternion(
            float[] rotations,
            int index,
            float x,
            float y,
            float z,
            out float outX,
            out float outY,
            out float outZ)
        {
            float qx = rotations[index * 4 + 0];
            float qy = rotations[index * 4 + 1];
            float qz = rotations[index * 4 + 2];
            float qw = rotations[index * 4 + 3];
            // v + 2w(q x v) + 2(q x (q x v))
            float cx = qy * z - qz * y;
            float cy = qz * x - qx * z;
            float cz = qx * y - qy * x;
            float ccx = qy * cz - qz * cy;
            float ccy = qz * cx - qx * cz;
            float ccz = qx * cy - qy * cx;
            outX = x + 2.0f * (qw * cx + ccx);
            outY = y + 2.0f * (qw * cy + ccy);
            outZ = z + 2.0f * (qw * cz + ccz);
        }

        private static void MultiplyQuaternions(
            float[] rotations,
            int leftIndex,
            float rx,
            float ry,
            float rz,
            float rw,
            float[] output,
            int outputIndex)
        {
            float lx = rotations[leftIndex * 4 + 0];
            float ly = rotations[leftIndex * 4 + 1];
            float lz = rotations[leftIndex * 4 + 2];
            float lw = rotations[leftIndex * 4 + 3];
            output[outputIndex * 4 + 0] = lw * rx + lx * rw + ly * rz - lz * ry;
            output[outputIndex * 4 + 1] = lw * ry - lx * rz + ly * rw + lz * rx;
            output[outputIndex * 4 + 2] = lw * rz + lx * ry - ly * rx + lz * rw;
            output[outputIndex * 4 + 3] = lw * rw - lx * rx - ly * ry - lz * rz;
        }

        public int IndexOf(string boneName)
        {
            if (Bones == null || boneName == null)
            {
                return -1;
            }
            for (int index = 0; index < Bones.Length; ++index)
            {
                if (Bones[index].Name == boneName)
                {
                    return index;
                }
            }
            return -1;
        }
    }

    /// <summary>
    /// Skeleton manifests read out of a gameplay catalog bundle.
    /// </summary>
    /// <remarks>
    /// Load this from whichever bundle bytes the session is actually running
    /// on: the packaged copy in host mode, or
    /// <see cref="Kernel.CopyGameplayCatalogBundle"/> in client mode, where the
    /// bytes came from the server. Reading the server's copy is what lets a
    /// skeleton be fixed and redeployed without rebuilding the Unity package.
    /// </remarks>
    public static class KernelSkeletonManifestCatalog
    {
        private const string ManifestDirectory = "skeleton_assets/generated/";
        private const string ManifestSuffix = ".skeleton_manifest.json";

        private static readonly Dictionary<uint, KernelSkeletonManifest> ByAssetId =
            new Dictionary<uint, KernelSkeletonManifest>();

        /// <summary>Manifests currently loaded, keyed by skeleton asset id.</summary>
        public static IReadOnlyDictionary<uint, KernelSkeletonManifest> Manifests =>
            ByAssetId;

        public static void Clear()
        {
            ByAssetId.Clear();
        }

        /// <summary>
        /// Replaces the loaded set with every manifest in <paramref name="bundleBytes"/>.
        /// </summary>
        public static bool TryLoadFromBundle(byte[] bundleBytes, out string error)
        {
            if (bundleBytes == null || bundleBytes.Length == 0)
            {
                error = "Gameplay catalog bundle bytes are empty.";
                return false;
            }

            var loaded = new Dictionary<uint, KernelSkeletonManifest>();
            try
            {
                using (var stream = new MemoryStream(bundleBytes, false))
                using (var archive = new ZipArchive(stream, ZipArchiveMode.Read))
                {
                    foreach (ZipArchiveEntry entry in archive.Entries)
                    {
                        string path = entry.FullName.Replace('\\', '/');
                        if (!path.StartsWith(ManifestDirectory, StringComparison.Ordinal) ||
                            !path.EndsWith(ManifestSuffix, StringComparison.Ordinal))
                        {
                            continue;
                        }
                        string json;
                        using (var reader = new StreamReader(
                                   entry.Open(),
                                   Encoding.UTF8))
                        {
                            json = reader.ReadToEnd();
                        }
                        if (!TryParse(json, path, out KernelSkeletonManifest manifest, out error))
                        {
                            return false;
                        }
                        if (loaded.ContainsKey(manifest.AssetId))
                        {
                            error =
                                $"Duplicate skeleton asset id {manifest.AssetId} in bundle " +
                                $"({path}).";
                            return false;
                        }
                        loaded.Add(manifest.AssetId, manifest);
                    }
                }
            }
            catch (InvalidDataException exception)
            {
                error = $"Gameplay catalog bundle is not a readable archive: {exception.Message}";
                return false;
            }

            if (loaded.Count == 0)
            {
                error =
                    $"Gameplay catalog bundle contains no '{ManifestDirectory}*{ManifestSuffix}'.";
                return false;
            }

            ByAssetId.Clear();
            foreach (KeyValuePair<uint, KernelSkeletonManifest> pair in loaded)
            {
                ByAssetId.Add(pair.Key, pair.Value);
            }
            error = null;
            return true;
        }

        public static bool TryGet(uint assetId, out KernelSkeletonManifest manifest)
        {
            return ByAssetId.TryGetValue(assetId, out manifest);
        }

        /// <summary>
        /// Looks a manifest up and additionally requires the content hash to
        /// match, which is what catches a skeleton whose asset was rebuilt
        /// while a scene still holds the old bone layout.
        /// </summary>
        public static bool TryGet(
            uint assetId,
            ulong contentHash,
            out KernelSkeletonManifest manifest)
        {
            if (!ByAssetId.TryGetValue(assetId, out manifest))
            {
                return false;
            }
            if (contentHash != 0ul && manifest.ContentHash != contentHash)
            {
                manifest = null;
                return false;
            }
            return true;
        }

        internal static bool TryParse(
            string json,
            string path,
            out KernelSkeletonManifest manifest,
            out string error)
        {
            manifest = null;
            if (!KernelJson.TryParse(json, out KernelJson.Value root, out string parseError))
            {
                error = $"{path}: {parseError}";
                return false;
            }
            if (root.Kind != KernelJson.ValueKind.Object)
            {
                error = $"{path}: manifest root is not an object.";
                return false;
            }

            var parsed = new KernelSkeletonManifest();
            if (!root.TryGetNumber("manifest_version", out double version) ||
                (version != 1.0 && version != 2.0))
            {
                error = $"{path}: unsupported or missing manifest_version.";
                return false;
            }
            parsed.ManifestVersion = (uint)version;

            if (!root.TryGetNumber("asset_id", out double assetId) || assetId <= 0.0)
            {
                error = $"{path}: missing or invalid asset_id.";
                return false;
            }
            parsed.AssetId = (uint)assetId;

            if (!root.TryGetString("name", out string name))
            {
                error = $"{path}: missing name.";
                return false;
            }
            parsed.Name = name;

            if (!root.TryGetString("content_hash", out string contentHashText) ||
                !TryParseHex64(contentHashText, out ulong contentHash) ||
                contentHash == 0ul)
            {
                error = $"{path}: missing or invalid content_hash.";
                return false;
            }
            parsed.ContentHash = contentHash;

            if (!root.TryGetNumber("bone_count", out double boneCount) ||
                !root.TryGetArray("bones", out List<KernelJson.Value> bones) ||
                bones.Count != (int)boneCount)
            {
                error = $"{path}: bone_count does not match the bones array.";
                return false;
            }

            var result = new KernelSkeletonManifestBone[bones.Count];
            for (int index = 0; index < bones.Count; ++index)
            {
                KernelJson.Value bone = bones[index];
                if (!bone.TryGetNumber("index", out double boneIndex) ||
                    (int)boneIndex != index ||
                    !bone.TryGetString("name", out string boneName) ||
                    string.IsNullOrEmpty(boneName) ||
                    !bone.TryGetNumber("parent_index", out double parentIndex))
                {
                    error = $"{path}: bone {index} is malformed.";
                    return false;
                }
                // The kernel indexes bones in this order and walks parents
                // before children, so a forward reference would break any
                // hierarchy rebuilt from it.
                if ((int)parentIndex >= index || (int)parentIndex < -1)
                {
                    error =
                        $"{path}: bone {index} ('{boneName}') has parent_index " +
                        $"{(int)parentIndex}, which is not an earlier bone.";
                    return false;
                }

                result[index] = new KernelSkeletonManifestBone
                {
                    Name = boneName,
                    ParentIndex = (int)parentIndex,
                    RestRotationW = 1.0f,
                    RestScaleX = 1.0f,
                    RestScaleY = 1.0f,
                    RestScaleZ = 1.0f,
                };

                if (parsed.ManifestVersion < 2u)
                {
                    continue;
                }
                if (!TryReadFloats(bone, "rest_translation", 3, out float[] translation) ||
                    !TryReadFloats(bone, "rest_rotation", 4, out float[] rotation) ||
                    !TryReadFloats(bone, "rest_scale", 3, out float[] scale))
                {
                    error =
                        $"{path}: bone {index} ('{boneName}') is missing a well-formed " +
                        "rest pose, which manifest_version 2 requires.";
                    return false;
                }
                result[index].RestPositionX = translation[0];
                result[index].RestPositionY = translation[1];
                result[index].RestPositionZ = translation[2];
                result[index].RestRotationX = rotation[0];
                result[index].RestRotationY = rotation[1];
                result[index].RestRotationZ = rotation[2];
                result[index].RestRotationW = rotation[3];
                result[index].RestScaleX = scale[0];
                result[index].RestScaleY = scale[1];
                result[index].RestScaleZ = scale[2];
            }

            for (int index = 0; index < result.Length; ++index)
            {
                for (int previous = 0; previous < index; ++previous)
                {
                    if (result[index].Name == result[previous].Name)
                    {
                        error =
                            $"{path}: bone name '{result[index].Name}' is not unique, " +
                            "so bones cannot be mapped by name.";
                        return false;
                    }
                }
            }

            parsed.Bones = result;
            manifest = parsed;
            error = null;
            return true;
        }

        private static bool TryReadFloats(
            KernelJson.Value owner,
            string key,
            int count,
            out float[] values)
        {
            values = null;
            if (!owner.TryGetArray(key, out List<KernelJson.Value> array) ||
                array.Count != count)
            {
                return false;
            }
            var result = new float[count];
            for (int index = 0; index < count; ++index)
            {
                if (array[index].Kind != KernelJson.ValueKind.Number)
                {
                    return false;
                }
                result[index] = (float)array[index].Number;
            }
            values = result;
            return true;
        }

        private static bool TryParseHex64(string text, out ulong value)
        {
            value = 0ul;
            if (string.IsNullOrEmpty(text))
            {
                return false;
            }
            string digits = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                ? text.Substring(2)
                : text;
            return ulong.TryParse(
                digits,
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture,
                out value);
        }
    }
}
