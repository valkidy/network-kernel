using System;

namespace NetworkExample.Kernel.Client
{
    public sealed class NetworkClient : IDisposable
    {
        private const uint ClientPredictionSessionFailureCode = 31;

        private readonly Kernel kernel;
        private bool disconnected;
        private uint localPeerId;
        private uint localPlayerNetId;
        private bool catalogSyncActive;
        private string catalogSyncAddress;
        private GameplayCatalogSyncOptions catalogSyncOptions;
        private GameplayCatalogSyncResult catalogSyncResult =
            new GameplayCatalogSyncResult();
        private NetworkClientConnectionState connectionState =
            NetworkClientConnectionState.Idle;

        public NetworkClient()
            : this(KernelConfig.CreateDefault(KernelMode.Client))
        {
        }

        public NetworkClient(KernelConfig config)
        {
            config.mode = KernelMode.Client;
            kernel = new Kernel(config);
        }

        public bool IsReady => localPeerId != 0 && localPlayerNetId != 0 && !disconnected;
        public bool IsDisconnected => disconnected;
        public uint LocalPeerId => localPeerId;
        public uint LocalPlayerNetId => localPlayerNetId;
        public Kernel Kernel => kernel;
        public NetworkClientConnectionState ConnectionState => connectionState;
        public GameplayCatalogSyncResult CatalogSyncResult => catalogSyncResult;

        public bool Start(string address)
        {
            ResetManagedSessionState();
            connectionState = NetworkClientConnectionState.Idle;
            return kernel.StartClient(address);
        }

        public bool Start(string address, GameplayCatalogSyncOptions syncOptions)
        {
            if (syncOptions == null)
            {
                throw new ArgumentNullException(nameof(syncOptions));
            }
            if (syncOptions.MaxBundleBytes == 0 ||
                syncOptions.MaxBundleBytes > int.MaxValue)
            {
                throw new ArgumentOutOfRangeException(
                    nameof(syncOptions),
                    "MaxBundleBytes must fit a managed byte array.");
            }

            double timeoutMilliseconds = syncOptions.Timeout.TotalMilliseconds;
            if (timeoutMilliseconds <= 0 || timeoutMilliseconds > uint.MaxValue)
            {
                throw new ArgumentOutOfRangeException(
                    nameof(syncOptions),
                    "Timeout must be positive and fit the native millisecond limit.");
            }

            catalogSyncAddress = address;
            catalogSyncOptions = syncOptions;
            catalogSyncResult = new GameplayCatalogSyncResult();
            ResetManagedSessionState();
            connectionState = NetworkClientConnectionState.FetchingManifest;
            catalogSyncActive = kernel.StartClientCatalogSync(
                address,
                (uint)syncOptions.MaxBundleBytes,
                (uint)Math.Ceiling(timeoutMilliseconds));
            if (!catalogSyncActive)
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidState,
                    "Kernel_StartClientCatalogSync failed.");
            }
            return catalogSyncActive;
        }

        public bool LoadGameplayCatalogFromMemory(
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult result)
        {
            return kernel.LoadGameplayCatalogFromMemory(bundleBytes, entryPath, out result);
        }

        public uint Update(float deltaSeconds, KernelEvent[] events)
        {
            kernel.Update(deltaSeconds);
            if (catalogSyncActive)
            {
                AdvanceCatalogSync();
            }
            uint eventCount = kernel.PollEvents(events);
            ApplyEvents(events, eventCount);
            return eventCount;
        }

        public uint GetRenderStates(RenderEntityState[] states)
        {
            return kernel.GetRenderStates(states);
        }

        public uint GetRenderStatesAtTime(ulong clientRenderTimeUs, RenderEntityState[] states)
        {
            return kernel.GetRenderStatesAtTime(clientRenderTimeUs, states);
        }

        public bool TrySubmitInput(KernelPlayerInput input)
        {
            if (!IsReady)
            {
                return false;
            }

            kernel.SubmitInput(localPeerId, input);
            return true;
        }

        public void Dispose()
        {
            kernel.Dispose();
        }

        private void ApplyEvents(KernelEvent[] events, uint eventCount)
        {
            if (events == null)
            {
                return;
            }

            int count = Math.Min(events.Length, (int)eventCount);
            for (int index = 0; index < count; ++index)
            {
                KernelEvent kernelEvent = events[index];
                if (kernelEvent.type == KernelEventType.PlayerJoined)
                {
                    RefreshLocalPlayerInfo();
                }
                else if (
                    kernelEvent.type == KernelEventType.Error &&
                    kernelEvent.code == ClientPredictionSessionFailureCode)
                {
                    catalogSyncActive = false;
                    disconnected = false;
                    localPeerId = 0;
                    localPlayerNetId = 0;
                    connectionState = NetworkClientConnectionState.Failed;
                }
                else if (kernelEvent.type == KernelEventType.Disconnected)
                {
                    disconnected = true;
                    localPeerId = 0;
                    localPlayerNetId = 0;
                    if (catalogSyncActive &&
                        connectionState != NetworkClientConnectionState.Failed)
                    {
                        connectionState = NetworkClientConnectionState.Disconnected;
                        catalogSyncResult.Error =
                            KernelGameplayCatalogSyncError.Disconnected;
                        catalogSyncResult.ErrorMessage = "Client disconnected.";
                    }
                }
            }
        }

        private void ResetManagedSessionState()
        {
            catalogSyncActive = false;
            disconnected = false;
            localPeerId = 0;
            localPlayerNetId = 0;
        }

        private void AdvanceCatalogSync()
        {
            if (!kernel.TryGetGameplayCatalogSyncStatus(
                    out KernelGameplayCatalogSyncStatus status))
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidState,
                    "Kernel_GetGameplayCatalogSyncStatus failed.");
                return;
            }

            if (status.manifest.bundle_size != 0)
            {
                catalogSyncResult.Manifest = status.manifest;
            }

            switch (status.state)
            {
                case KernelGameplayCatalogSyncState.Connecting:
                case KernelGameplayCatalogSyncState.FetchingManifest:
                    connectionState = NetworkClientConnectionState.FetchingManifest;
                    break;
                case KernelGameplayCatalogSyncState.ManifestReady:
                    if (connectionState != NetworkClientConnectionState.CheckingCache &&
                        connectionState != NetworkClientConnectionState.DownloadingCatalog &&
                        connectionState != NetworkClientConnectionState.LoadingCatalog &&
                        connectionState != NetworkClientConnectionState.Handshaking)
                    {
                        ProcessManifest(status.manifest);
                    }
                    break;
                case KernelGameplayCatalogSyncState.Downloading:
                    connectionState = NetworkClientConnectionState.DownloadingCatalog;
                    break;
                case KernelGameplayCatalogSyncState.BundleReady:
                    if (connectionState != NetworkClientConnectionState.LoadingCatalog &&
                        connectionState != NetworkClientConnectionState.Handshaking)
                    {
                        ProcessDownloadedBundle(status.manifest);
                    }
                    break;
                case KernelGameplayCatalogSyncState.Handshaking:
                    connectionState = NetworkClientConnectionState.Handshaking;
                    break;
                case KernelGameplayCatalogSyncState.Ready:
                    connectionState = NetworkClientConnectionState.Ready;
                    break;
                case KernelGameplayCatalogSyncState.Failed:
                    FailCatalogSync(status.error, $"Native catalog sync failed: {status.error}.");
                    break;
                case KernelGameplayCatalogSyncState.Disconnected:
                    connectionState = NetworkClientConnectionState.Disconnected;
                    catalogSyncResult.Error = KernelGameplayCatalogSyncError.Disconnected;
                    catalogSyncResult.ErrorMessage = "Client disconnected during catalog sync.";
                    break;
            }
        }

        private void ProcessManifest(KernelGameplayCatalogManifest manifest)
        {
            connectionState = NetworkClientConnectionState.CheckingCache;
            catalogSyncResult.Manifest = manifest;
            if (!ValidateManifest(manifest, out string validationError))
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidManifest,
                    validationError);
                return;
            }

            if (GameplayCatalogCache.TryRead(
                    catalogSyncOptions.CacheDirectory,
                    catalogSyncAddress,
                    manifest,
                    out byte[] cachedBytes,
                    out string warning))
            {
                catalogSyncResult.CacheHit = true;
                LoadCatalogAndContinue(cachedBytes, manifest, false);
                return;
            }

            if (!string.IsNullOrEmpty(warning))
            {
                catalogSyncResult.CacheWarning = warning;
            }
            if (!kernel.RequestGameplayCatalogBundle())
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.Transport,
                    "Kernel_RequestGameplayCatalogBundle failed.");
                return;
            }
            connectionState = NetworkClientConnectionState.DownloadingCatalog;
        }

        private void ProcessDownloadedBundle(KernelGameplayCatalogManifest manifest)
        {
            if (!ValidateManifest(manifest, out string validationError))
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidManifest,
                    validationError);
                return;
            }

            var bundleBytes = new byte[manifest.bundle_size];
            if (!kernel.CopyGameplayCatalogBundle(bundleBytes, out uint copiedSize) ||
                copiedSize != manifest.bundle_size)
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidBundle,
                    "Kernel_CopyGameplayCatalogBundle failed or returned an invalid size.");
                return;
            }
            LoadCatalogAndContinue(bundleBytes, manifest, true);
        }

        private void LoadCatalogAndContinue(
            byte[] bundleBytes,
            KernelGameplayCatalogManifest manifest,
            bool writeCache)
        {
            connectionState = NetworkClientConnectionState.LoadingCatalog;
            if (!GameplayCatalogCache.MatchesManifest(bundleBytes, manifest))
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidBundle,
                    "Gameplay catalog bundle size or SHA-256 did not match the manifest.");
                return;
            }

            bool loaded = kernel.LoadGameplayCatalogFromMemory(
                bundleBytes,
                manifest.entry_path,
                out KernelGameplayCatalogLoadResult loadResult);
            catalogSyncResult.LoadResult = loadResult;
            if (!loaded ||
                loadResult.status != KernelConstants.GameplayCatalogLoadStatusSuccess)
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidBundle,
                    string.IsNullOrEmpty(loadResult.diagnostic)
                        ? "Gameplay catalog bundle load failed."
                        : loadResult.diagnostic);
                return;
            }
            if (loadResult.catalog_version != manifest.catalog_version ||
                loadResult.catalog_hash != manifest.catalog_hash)
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.VersionMismatch,
                    "Loaded gameplay catalog version or hash did not match the server manifest.");
                return;
            }

            if (writeCache &&
                !GameplayCatalogCache.TryWrite(
                    catalogSyncOptions.CacheDirectory,
                    catalogSyncAddress,
                    manifest,
                    bundleBytes,
                    out string warning))
            {
                catalogSyncResult.MemoryOnly = true;
                catalogSyncResult.CacheWarning = warning;
            }

            if (!kernel.ContinueClientHandshake())
            {
                FailCatalogSync(
                    KernelGameplayCatalogSyncError.InvalidState,
                    "Kernel_ContinueClientHandshake failed.");
                return;
            }
            connectionState = NetworkClientConnectionState.Handshaking;
        }

        private bool ValidateManifest(
            KernelGameplayCatalogManifest manifest,
            out string error)
        {
            if (manifest.bundle_size == 0 ||
                manifest.bundle_size > catalogSyncOptions.MaxBundleBytes)
            {
                error =
                    $"Gameplay catalog bundle size {manifest.bundle_size} exceeds " +
                    $"the configured limit {catalogSyncOptions.MaxBundleBytes}.";
                return false;
            }
            if (string.IsNullOrEmpty(manifest.entry_path))
            {
                error = "Gameplay catalog manifest entry path was empty.";
                return false;
            }
            if (manifest.bundle_sha256 == null ||
                manifest.bundle_sha256.Length != KernelConstants.GameplayCatalogSha256Size)
            {
                error = "Gameplay catalog manifest SHA-256 was invalid.";
                return false;
            }

            error = null;
            return true;
        }

        private void FailCatalogSync(
            KernelGameplayCatalogSyncError error,
            string message)
        {
            catalogSyncActive = false;
            connectionState = NetworkClientConnectionState.Failed;
            catalogSyncResult.Error = error;
            catalogSyncResult.ErrorMessage = message;
        }

        private void RefreshLocalPlayerInfo()
        {
            if (!kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) ||
                info.connected == 0)
            {
                return;
            }

            disconnected = false;
            localPeerId = info.peer_id;
            localPlayerNetId = info.player_net_id;
        }
    }
}
