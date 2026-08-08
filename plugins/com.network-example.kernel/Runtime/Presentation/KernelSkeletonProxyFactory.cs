using UnityEngine;

namespace NetworkExample.Kernel.Presentation
{
    /// <summary>
    /// Builds a stand-in rig from a skeleton manifest, for showing Native bone
    /// poses without importing the source mesh.
    /// </summary>
    /// <remarks>
    /// The hierarchy is created straight from the manifest's rest pose, which
    /// makes it aligned with the kernel by construction rather than by matching
    /// an importer's conventions. Nothing in the pipeline converts coordinates
    /// -- the exporter writes Unity's own local TRS, gltf2ozz copies it into ozz
    /// verbatim, and the kernel hands bone transforms back the same way -- so
    /// these transforms are the ones the kernel solves against. An importer that
    /// converts handedness, as the common glTF ones do, would break that.
    ///
    /// Geometry is whatever the rig carries in its bone scales. On rigs authored
    /// as scaled primitives, a GEO_ bone's rest scale is exactly its box, so the
    /// proxy reproduces the shape. On rigs that bake dimensions into mesh
    /// vertices instead, those bones have unit scale and render as small
    /// markers, because the manifest does not carry mesh bounds.
    /// </remarks>
    public static class KernelSkeletonProxyFactory
    {
        private const string GeometryPrefix = "GEO_";

        /// <summary>Joint marker edge, as a fraction of the rig's height.</summary>
        private const float JointMarkerScale = 0.02f;

        private static readonly Color GeometryColor = new Color(0.2f, 0.7f, 0.78f, 1.0f);
        private static readonly Color JointColor = new Color(0.95f, 0.62f, 0.25f, 1.0f);

        public static GameObject TryCreate(uint skeletonAssetId, out string error)
        {
            if (!KernelSkeletonManifestCatalog.TryGet(
                    skeletonAssetId,
                    out KernelSkeletonManifest manifest))
            {
                error =
                    $"No skeleton manifest for asset {skeletonAssetId}. Load the " +
                    "gameplay catalog bundle first.";
                return null;
            }
            return TryCreate(manifest, out error);
        }

        public static GameObject TryCreate(
            KernelSkeletonManifest manifest,
            out string error)
        {
            if (manifest == null || manifest.BoneCount == 0)
            {
                error = "Skeleton manifest is empty.";
                return null;
            }
            if (!manifest.HasRestPose)
            {
                error =
                    $"Skeleton manifest '{manifest.Name}' is version " +
                    $"{manifest.ManifestVersion} and carries no rest pose; " +
                    "a proxy needs manifest_version 2 or newer.";
                return null;
            }

            var root = new GameObject($"{manifest.Name}_proxy");
            var bones = new Transform[manifest.BoneCount];
            for (int index = 0; index < manifest.BoneCount; ++index)
            {
                KernelSkeletonManifestBone bone = manifest.Bones[index];
                var boneObject = new GameObject(bone.Name);
                Transform parent = bone.ParentIndex < 0
                    ? root.transform
                    : bones[bone.ParentIndex];
                boneObject.transform.SetParent(parent, false);
                boneObject.transform.localPosition = new Vector3(
                    bone.RestPositionX, bone.RestPositionY, bone.RestPositionZ);
                boneObject.transform.localRotation = new Quaternion(
                    bone.RestRotationX, bone.RestRotationY,
                    bone.RestRotationZ, bone.RestRotationW);
                boneObject.transform.localScale = new Vector3(
                    bone.RestScaleX, bone.RestScaleY, bone.RestScaleZ);
                bones[index] = boneObject.transform;
            }

            float markerEdge = JointMarkerScale * RigHeight(manifest);
            for (int index = 0; index < manifest.BoneCount; ++index)
            {
                KernelSkeletonManifestBone bone = manifest.Bones[index];
                bool isGeometry = bone.Name.StartsWith(
                    GeometryPrefix,
                    System.StringComparison.Ordinal);
                bool carriesShape = !Mathf.Approximately(bone.RestScaleX, 1.0f) ||
                    !Mathf.Approximately(bone.RestScaleY, 1.0f) ||
                    !Mathf.Approximately(bone.RestScaleZ, 1.0f);

                if (isGeometry && carriesShape)
                {
                    // The bone's own scale is the box, so a unit cube parented
                    // to it inherits the authored dimensions exactly.
                    AttachPrimitive(bones[index], Vector3.one, GeometryColor, "Shape");
                    continue;
                }
                // Everything else gets a marker sized to the rig rather than to
                // the bone, and divided out of the bone's scale so a scaled
                // parent does not stretch it.
                Vector3 boneScale = bones[index].localScale;
                AttachPrimitive(
                    bones[index],
                    new Vector3(
                        SafeDivide(markerEdge, boneScale.x),
                        SafeDivide(markerEdge, boneScale.y),
                        SafeDivide(markerEdge, boneScale.z)),
                    isGeometry ? GeometryColor : JointColor,
                    "Marker");
            }

            var binding = root.AddComponent<KernelSkeletonBinding>();
            binding.SkeletonAssetId = manifest.AssetId;
            binding.SkeletonContentHash = manifest.ContentHash;
            binding.AutoMapKnownSkeleton = false;
            // The hierarchy came from the manifest's own rest pose, so the
            // Native bind pose and this one are the same numbers; applying
            // deltas on top would double-count.
            binding.PreservePrefabBindPose = false;
            binding.Bones = bones;
            root.AddComponent<KernelSkeletonPoseApplicator>();
            error = null;
            return root;
        }

        /// <summary>Model-space height of the rest pose, for scaling markers.</summary>
        private static float RigHeight(KernelSkeletonManifest manifest)
        {
            float[] positions = manifest.ComputeRestModelPositions();
            float minimum = float.MaxValue;
            float maximum = float.MinValue;
            for (int index = 0; index < manifest.BoneCount; ++index)
            {
                float y = positions[index * 3 + 1];
                minimum = Mathf.Min(minimum, y);
                maximum = Mathf.Max(maximum, y);
            }
            float height = maximum - minimum;
            return height > 0.0001f ? height : 1.0f;
        }

        private static float SafeDivide(float value, float divisor)
        {
            return Mathf.Abs(divisor) > 0.0001f ? value / divisor : value;
        }

        private static void AttachPrimitive(
            Transform parent,
            Vector3 localScale,
            Color color,
            string name)
        {
            GameObject primitive = GameObject.CreatePrimitive(PrimitiveType.Cube);
            primitive.name = name;
            primitive.transform.SetParent(parent, false);
            primitive.transform.localPosition = Vector3.zero;
            primitive.transform.localRotation = Quaternion.identity;
            primitive.transform.localScale = localScale;
            Collider collider = primitive.GetComponent<Collider>();
            if (collider != null)
            {
                Object.Destroy(collider);
            }
            var renderer = primitive.GetComponent<MeshRenderer>();
            if (renderer != null)
            {
                renderer.material.color = color;
            }
        }
    }
}
