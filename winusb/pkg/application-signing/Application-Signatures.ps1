# SPDX-License-Identifier: GPL-2.0-only
# Windows PowerShell 5.1. Cryptographic verification without changing trust stores.
# Adapted from BonDriver_CECHZD1J 52dd600; signature integrity is independent of
# certificate-store trust. The publisher is separate from the existing PX4 CAT CA.
function Get-Px4ApplicationHash([string]$Path) {
    # Windows PowerShell の Get-FileHash はモジュール側の WhatIf に影響されるため、
    # 読み取り専用の .NET API で検証を必ず実行する。
    $algorithm=[Security.Cryptography.SHA256]::Create();$file=$null
    try {
        $file=[IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($algorithm.ComputeHash($file)).Replace('-','')
    }finally{if($file){$file.Dispose()};$algorithm.Dispose()}
}
function Get-Px4ApplicationPublisher([string]$CertificatePath) {
    $expected='5B6288396CA6D34EFFE2B1DBFDA3B6EA8B861E08FBFDD7F83356AA1EC594A1EE'
    if((Get-Px4ApplicationHash $CertificatePath) -cne $expected){throw 'Unexpected application publisher certificate.'}
    return [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
}
function Get-Px4ApplicationFiles([string]$Directory) {
    foreach($name in @('DriverHost_PX4.exe','WinSCard.dll')){
        if(-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)){throw "Missing required binary: $name"}
    }
    $bon=@(Get-ChildItem -LiteralPath $Directory -File | Where-Object {$_.Name -match '^BonDriver_[A-Za-z0-9_-]+\.dll$'})
    if(-not $bon.Count){throw 'No BonDriver DLL in the distribution folder.'}
    $required=@('DriverHost_PX4.exe','WinSCard.dll')+@($bon.Name)
    $all=@(Get-ChildItem -LiteralPath $Directory -File | Where-Object {$_.Extension -in '.exe','.dll'} | ForEach-Object {$_.Name})
    if(@(Compare-Object $required $all).Count){throw 'Unexpected executable in the distribution folder.'}
    return @($required | Sort-Object)
}
function Test-Px4ApplicationSignatures([string]$Directory,[string]$CertificatePath='') {
    $directory=(Resolve-Path -LiteralPath $Directory).Path
    if(-not $CertificatePath){$CertificatePath=Join-Path $directory 'Publisher.cer'}
    $publisher=Get-Px4ApplicationPublisher $CertificatePath
    try {
        Initialize-Px4ApplicationVerifier
        $manifest=Get-Content -LiteralPath (Join-Path $directory 'application-signatures.json') -Raw | ConvertFrom-Json
        $required=@(Get-Px4ApplicationFiles $directory)
        $names=@($manifest.Files.PSObject.Properties.Name)
        if($manifest.Schema -ne 1 -or $manifest.CertificateSHA256 -cne (Get-Px4ApplicationHash $CertificatePath)){throw 'Unexpected application signature manifest identity.'}
        if($names.Count -ne $required.Count -or @(Compare-Object $required $names).Count){throw 'Unexpected application signature manifest file set.'}
        foreach($name in $required){
            $file=Join-Path $directory $name
            if((Get-Px4ApplicationHash $file) -cne $manifest.Files.$name){throw "Application hash mismatch: $name"}
            [Px4ApplicationVerifier]::Verify($file,$publisher.RawData)
        }
        Write-Host "PASS: application signatures, PE digests and pinned publisher: $directory"
    }finally{$publisher.Dispose()}
}
function Initialize-Px4ApplicationVerifier {
    if($null -ne ('Px4ApplicationVerifier' -as [type])) {return}
    Add-Type -AssemblyName System.Security
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography.Pkcs;
public static class Px4ApplicationVerifier {
    [StructLayout(LayoutKind.Sequential)] struct Blob { public uint Size; public IntPtr Data; }
    [StructLayout(LayoutKind.Sequential)] struct Algorithm { public IntPtr Oid; public Blob Parameters; }
    [StructLayout(LayoutKind.Sequential)] struct Attribute { public IntPtr Oid; public Blob Value; }
    [StructLayout(LayoutKind.Sequential)] struct Indirect { public Attribute Data; public Algorithm Algorithm; public Blob Digest; }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] class Subject {
        public uint Size;
        public IntPtr Type, File;
        [MarshalAs(UnmanagedType.LPWStr)] public string Name;
        [MarshalAs(UnmanagedType.LPWStr)] public string Display;
        public uint Reserved1, Version;
        public IntPtr Provider;
        public Algorithm Algorithm;
        public uint Flags, Encoding, Reserved2, CapiSettings, SecuritySettings, Index, UnionChoice;
        public IntPtr AddInfo, ClientData;
    }
    [DllImport("crypt32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool CryptSIPRetrieveSubjectGuid(string name,IntPtr file,out Guid subject);
    [DllImport("crypt32.dll",SetLastError=true)]
    static extern bool CryptSIPGetSignedDataMsg([In,Out] Subject subject,out uint encoding,uint index,ref uint size,byte[] data);
    [DllImport("crypt32.dll",SetLastError=true)]
    static extern bool CryptDecodeObjectEx(uint encoding,IntPtr type,byte[] data,uint size,uint flags,IntPtr parameters,out IntPtr result,out uint resultSize);
    [DllImport("crypt32.dll",SetLastError=true)]
    static extern bool CryptSIPVerifyIndirectData([In,Out] Subject subject,IntPtr indirect);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    static void Check(bool ok,string operation) { if(!ok) throw new Win32Exception(Marshal.GetLastWin32Error(),operation); }
    public static void Verify(string path,byte[] expectedSigner) {
        // Keeping one read handle also prevents replacement or modification
        // between extraction of the signed message and the SIP file-hash check.
        using(var file=File.Open(path,FileMode.Open,FileAccess.Read,FileShare.Read)) {
            Guid type;Check(CryptSIPRetrieveSubjectGuid(path,file.SafeFileHandle.DangerousGetHandle(),out type),"Unknown signed-file format");
            IntPtr typeMemory=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Guid)));
            try {
                Marshal.StructureToPtr(type,typeMemory,false);
                var subject=new Subject { Size=(uint)Marshal.SizeOf(typeof(Subject)),Type=typeMemory,
                    File=file.SafeFileHandle.DangerousGetHandle(),Name=path };
                uint encoding,size=0;
                Check(CryptSIPGetSignedDataMsg(subject,out encoding,0,ref size,null),"Missing embedded Authenticode signature");
                if(size==0 || size>1024*1024) throw new InvalidDataException("Unexpected signature size.");
                byte[] message=new byte[size];
                Check(CryptSIPGetSignedDataMsg(subject,out encoding,0,ref size,message),"Cannot read Authenticode signature");
                var cms=new SignedCms();cms.Decode(message);
                if(cms.ContentInfo.ContentType.Value!="1.3.6.1.4.1.311.2.1.4" || cms.SignerInfos.Count!=1)
                    throw new InvalidDataException("Expected one Authenticode signer.");
                if(cms.SignerInfos[0].DigestAlgorithm.Value!="2.16.840.1.101.3.4.2.1")
                    throw new InvalidDataException("Expected SHA-256 signing.");
                cms.CheckSignature(true); // Integrity only; no trust-store import.
                if(Convert.ToBase64String(cms.SignerInfos[0].Certificate.RawData)!=Convert.ToBase64String(expectedSigner))
                    throw new InvalidDataException("Binary signer differs from the pinned application publisher.");
                IntPtr indirect;uint decodedSize;
                Check(CryptDecodeObjectEx(0x10001,new IntPtr(2003),cms.ContentInfo.Content,(uint)cms.ContentInfo.Content.Length,
                    0x8000,IntPtr.Zero,out indirect,out decodedSize),"Invalid Authenticode digest data");
                try {
                    var data=(Indirect)Marshal.PtrToStructure(indirect,typeof(Indirect));
                    if(Marshal.PtrToStringAnsi(data.Algorithm.Oid)!="2.16.840.1.101.3.4.2.1")
                        throw new InvalidDataException("Expected SHA-256 file digest.");
                    subject.Algorithm=data.Algorithm;subject.Encoding=encoding;
                    Check(CryptSIPVerifyIndirectData(subject,indirect),"Signed PE file hash mismatch");
                } finally {LocalFree(indirect);}
            } finally {Marshal.FreeHGlobal(typeMemory);}
        }
    }
}
'@ -ReferencedAssemblies System.Security
}

