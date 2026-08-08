using System;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Host;
using NetworkExample.Kernel.Presentation;
using UnityEngine;

public sealed class NetworkKernelMonsterObserverBehaviour : MonoBehaviour
{
    private const string DefaultBundleResourcePath =
        "gameplay_catalog_bundle/bundle";
    private const string DefaultEntryPath =
        "monster_observer_gameplay_catalog.yaml";
    private const uint MonsterTemplateId = 20;

    [SerializeField]
    private ushort port = 7777;

    [SerializeField]
    private TextAsset gameplayCatalogBundle;

    [SerializeField]
    private string gameplayCatalogEntryPath = DefaultEntryPath;

    [SerializeField]
    private KernelEntityPresentationWorld presentationWorld;

    [SerializeField]
    private Camera observerCamera;

    [SerializeField]
    private Vector3 cameraOffset = new Vector3(18.0f, 14.0f, -24.0f);

    [SerializeField]
    private bool createReferenceGround = true;

    // Which skeleton each templated rig uses. The bone layout itself comes from
    // the manifest; this only says which manifest to ask for.
    private static readonly System.Collections.Generic.Dictionary<uint, uint>
        skeletonAssetIdByTemplateId =
            new System.Collections.Generic.Dictionary<uint, uint>
            {
                { MonsterTemplateId, 1u },
                { 21u, 4u },  // quadruped_actor
                { 22u, 5u },  // tripod_actor
                { 23u, 3u },  // biped_actor
            };

    private readonly RenderEntityState[] rootStates = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private NetworkHost host;
    private GameObject referenceGround;
    private ulong clientRenderTimeUs;
    private uint logFrame;

    private void Start()
    {
        presentationWorld = presentationWorld != null
            ? presentationWorld
            : GetComponent<KernelEntityPresentationWorld>();
        if (presentationWorld == null)
        {
            presentationWorld = gameObject.AddComponent<KernelEntityPresentationWorld>();
        }
        presentationWorld.FallbackFactory = CreatePresentationProxy;

        if (createReferenceGround)
        {
            referenceGround = CreateGround();
        }
        ResolveCamera();

        TextAsset bundle = gameplayCatalogBundle != null
            ? gameplayCatalogBundle
            : Resources.Load<TextAsset>(DefaultBundleResourcePath);
        if (bundle == null)
        {
            Fail(
                "Monster Observer could not load gameplay catalog bundle resource '" +
                DefaultBundleResourcePath + "'.");
            return;
        }

        // Read the bone layouts out of the same bytes the host is about to load,
        // so the rig the observer draws and the rig the kernel poses can never
        // be two different versions of the skeleton.
        if (!KernelSkeletonManifestCatalog.TryLoadFromBundle(
                bundle.bytes,
                out string manifestError))
        {
            Fail($"Monster Observer could not read skeleton manifests: {manifestError}");
            return;
        }

        host = new NetworkHost();
        try
        {
            bool started = host.Start(
                port,
                new GameplayCatalogServerOptions
                {
                    BundleBytes = bundle.bytes,
                    EntryPath = gameplayCatalogEntryPath,
                    ContentNamespace = "monster_observer",
                    EnableClientSync = true,
                },
                out KernelGameplayCatalogLoadResult result);
            if (!started)
            {
                Fail($"Monster Observer failed to listen on port {port}.");
                return;
            }

            Debug.Log(
                $"Monster Observer loaded catalog version={result.catalog_version} " +
                $"hash=0x{result.catalog_hash:x16} entry={gameplayCatalogEntryPath}.");
        }
        catch (Exception exception)
        {
            Fail(
                $"Monster Observer failed to load '{gameplayCatalogEntryPath}': " +
                exception.Message);
        }
    }

    private void Update()
    {
        if (host == null || !host.IsRunning)
        {
            return;
        }

        float deltaSeconds = Time.deltaTime;
        clientRenderTimeUs += SecondsToMicroseconds(deltaSeconds);
        host.Update(deltaSeconds, events);

        uint rootStateCount = presentationWorld.Present(
            host.Kernel,
            clientRenderTimeUs,
            rootStates);

        ++logFrame;
        if (logFrame % 120U == 0U)
        {
            Debug.Log(
                $"Monster Observer roots={rootStateCount} " +
                $"skeletons={presentationWorld.LastSkeletonStateCount} " +
                $"enemies={host.EnemyCount} " +
                $"local_player={host.LocalPlayerNetId}.");
        }
    }

    private void LateUpdate()
    {
        if (observerCamera == null || presentationWorld == null ||
            !presentationWorld.TryGetPresentationTransform(
                MonsterTemplateId,
                out Transform monster))
        {
            return;
        }

        Vector3 target = monster.position + Vector3.up * 2.5f;
        Vector3 desiredPosition = target + cameraOffset;
        observerCamera.transform.position = Vector3.Lerp(
            observerCamera.transform.position,
            desiredPosition,
            1.0f - Mathf.Exp(-4.0f * Time.deltaTime));
        observerCamera.transform.LookAt(target, Vector3.up);
    }

    private GameObject CreatePresentationProxy(RenderEntityState state)
    {
        // Any templated skeleton gets a rig built from its manifest, so adding
        // a rig to the catalog is enough to see it -- no table here to update.
        if (skeletonAssetIdByTemplateId.TryGetValue(
                state.template_id,
                out uint skeletonAssetId))
        {
            GameObject rig = KernelSkeletonProxyFactory.TryCreate(
                skeletonAssetId,
                out string error);
            if (rig != null)
            {
                return rig;
            }
            Debug.LogWarning(
                $"Falling back to a capsule for template {state.template_id}: {error}",
                this);
        }

        GameObject proxy = GameObject.CreatePrimitive(PrimitiveType.Capsule);
        proxy.name = $"Observer_Template_{state.template_id}";
        proxy.transform.localScale = new Vector3(1.0f, 2.0f, 1.0f);
        Collider collider = proxy.GetComponent<Collider>();
        if (collider != null)
        {
            Destroy(collider);
        }
        return proxy;
    }

    private void ResolveCamera()
    {
        if (observerCamera == null)
        {
            observerCamera = Camera.main;
        }
        if (observerCamera == null)
        {
            var cameraObject = new GameObject("Monster Observer Camera");
            observerCamera = cameraObject.AddComponent<Camera>();
        }

        observerCamera.transform.position = cameraOffset + Vector3.up * 2.5f;
        observerCamera.transform.LookAt(Vector3.up * 2.5f, Vector3.up);
    }

    private static GameObject CreateGround()
    {
        GameObject ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
        ground.name = "Monster Observer Reference Ground";
        ground.transform.position = Vector3.zero;
        ground.transform.localScale = new Vector3(5.0f, 1.0f, 5.0f);
        Collider collider = ground.GetComponent<Collider>();
        if (collider != null)
        {
            Destroy(collider);
        }
        SetColor(ground, new Color(0.2f, 0.24f, 0.2f, 1.0f));
        return ground;
    }

    private void Fail(string message)
    {
        Debug.LogError(message, this);
        host?.Dispose();
        host = null;
        enabled = false;
    }

    private static ulong SecondsToMicroseconds(float seconds)
    {
        return seconds <= 0.0f ? 0UL : (ulong)(seconds * 1000000.0f);
    }

    private static void SetColor(GameObject gameObject, Color color)
    {
        Renderer renderer = gameObject.GetComponent<Renderer>();
        if (renderer == null)
        {
            return;
        }

        var properties = new MaterialPropertyBlock();
        properties.SetColor("_BaseColor", color);
        properties.SetColor("_Color", color);
        renderer.SetPropertyBlock(properties);
    }

    private void OnDestroy()
    {
        presentationWorld?.DespawnAll();
        host?.Dispose();
        host = null;
        if (referenceGround != null)
        {
            Destroy(referenceGround);
        }
    }

}
