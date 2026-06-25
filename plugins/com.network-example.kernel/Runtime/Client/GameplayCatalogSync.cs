using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace NetworkExample.Kernel.Client
{
    public sealed class GameplayCatalogSyncOptions
    {
        public string CacheDirectory { get; set; }
        public ulong MaxBundleBytes { get; set; } =
            KernelConstants.GameplayCatalogSyncDefaultMaxBundleSize;
        public TimeSpan Timeout { get; set; } =
            TimeSpan.FromMilliseconds(KernelConstants.GameplayCatalogSyncDefaultTimeoutMs);
    }

    public enum NetworkClientConnectionState
    {
        Idle,
        FetchingManifest,
        CheckingCache,
        DownloadingCatalog,
        LoadingCatalog,
        Handshaking,
        Ready,
        Failed,
        Disconnected,
    }

    public sealed class GameplayCatalogSyncResult
    {
        public bool CacheHit { get; internal set; }
        public bool MemoryOnly { get; internal set; }
        public string CacheWarning { get; internal set; }
        public KernelGameplayCatalogSyncError Error { get; internal set; }
        public string ErrorMessage { get; internal set; }
        public KernelGameplayCatalogManifest Manifest { get; internal set; }
        public KernelGameplayCatalogLoadResult LoadResult { get; internal set; }
    }

    internal static class GameplayCatalogCache
    {
        internal static bool TryRead(
            string cacheDirectory,
            string address,
            KernelGameplayCatalogManifest manifest,
            out byte[] bundleBytes,
            out string warning)
        {
            bundleBytes = null;
            warning = null;
            if (string.IsNullOrWhiteSpace(cacheDirectory))
            {
                warning = "Gameplay catalog cache directory is not configured.";
                return false;
            }

            try
            {
                string path = GetBundlePath(cacheDirectory, address, manifest);
                if (!File.Exists(path))
                {
                    return false;
                }

                byte[] cachedBytes = File.ReadAllBytes(path);
                if (!MatchesManifest(cachedBytes, manifest))
                {
                    warning = $"Gameplay catalog cache entry failed validation: {path}";
                    TryDelete(path);
                    return false;
                }

                bundleBytes = cachedBytes;
                return true;
            }
            catch (Exception exception)
            {
                warning = $"Gameplay catalog cache read failed: {exception.Message}";
                return false;
            }
        }

        internal static bool TryWrite(
            string cacheDirectory,
            string address,
            KernelGameplayCatalogManifest manifest,
            byte[] bundleBytes,
            out string warning)
        {
            warning = null;
            if (string.IsNullOrWhiteSpace(cacheDirectory))
            {
                warning = "Gameplay catalog cache directory is not configured.";
                return false;
            }

            string temporaryPath = null;
            try
            {
                string path = GetBundlePath(cacheDirectory, address, manifest);
                string directory = Path.GetDirectoryName(path);
                Directory.CreateDirectory(directory);
                temporaryPath = path + ".tmp." + Guid.NewGuid().ToString("N");
                File.WriteAllBytes(temporaryPath, bundleBytes);

                if (File.Exists(path))
                {
                    try
                    {
                        File.Replace(temporaryPath, path, null);
                        temporaryPath = null;
                        return true;
                    }
                    catch (PlatformNotSupportedException)
                    {
                    }
                    catch (IOException)
                    {
                    }

                    File.Delete(path);
                }

                File.Move(temporaryPath, path);
                temporaryPath = null;
                return true;
            }
            catch (Exception exception)
            {
                warning = $"Gameplay catalog cache write failed: {exception.Message}";
                return false;
            }
            finally
            {
                if (!string.IsNullOrEmpty(temporaryPath))
                {
                    TryDelete(temporaryPath);
                }
            }
        }

        internal static bool MatchesManifest(
            byte[] bundleBytes,
            KernelGameplayCatalogManifest manifest)
        {
            if (bundleBytes == null ||
                bundleBytes.LongLength != manifest.bundle_size ||
                manifest.bundle_sha256 == null ||
                manifest.bundle_sha256.Length != KernelConstants.GameplayCatalogSha256Size)
            {
                return false;
            }

            byte[] digest;
            using (SHA256 sha256 = SHA256.Create())
            {
                digest = sha256.ComputeHash(bundleBytes);
            }

            int difference = 0;
            for (int index = 0; index < digest.Length; ++index)
            {
                difference |= digest[index] ^ manifest.bundle_sha256[index];
            }
            return difference == 0;
        }

        private static string GetBundlePath(
            string cacheDirectory,
            string address,
            KernelGameplayCatalogManifest manifest)
        {
            string normalizedAddress = (address ?? string.Empty).Trim().ToLowerInvariant();
            string identitySource =
                normalizedAddress + "\n" + (manifest.content_namespace ?? string.Empty);
            byte[] identityBytes = Encoding.UTF8.GetBytes(identitySource);
            byte[] identityDigest;
            using (SHA256 sha256 = SHA256.Create())
            {
                identityDigest = sha256.ComputeHash(identityBytes);
            }

            return Path.Combine(
                Path.GetFullPath(cacheDirectory),
                ToHex(identityDigest),
                manifest.catalog_hash.ToString("x16"),
                "bundle.zip");
        }

        private static string ToHex(byte[] bytes)
        {
            var result = new StringBuilder(bytes.Length * 2);
            for (int index = 0; index < bytes.Length; ++index)
            {
                result.Append(bytes[index].ToString("x2"));
            }
            return result.ToString();
        }

        private static void TryDelete(string path)
        {
            try
            {
                File.Delete(path);
            }
            catch
            {
            }
        }
    }
}
