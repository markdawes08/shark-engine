# Windows 11 / Windows PowerShell 5.1. No Python, Visual Studio/C++ compiler,
# Vulkan SDK or checkout is needed; Add-Type uses Windows' built-in .NET tools.
# This is a development qualification fixture, not a game export or platform claim.
[CmdletBinding()]
param(
    [string]$Mode = '',
    [string]$Device = '',
    [string]$Output = '',
    [int]$TimeoutSeconds = 120,
    [int]$LifecycleCycles = 2,
    [int]$DeviceCycles = 1,
    [int]$PresentSeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$receipt = [ordered]@{
    schema_version = 1; status = 'FAIL'; qualification = 'NOT_RUN'
    scope = 'Packaged probe execution only; clean-machine and hardware qualification are separate'
    mode = $Mode; started_utc = [DateTime]::UtcNow.ToString('o'); exit_code = 2
    errors = @(); process = $null; report = $null
}
$outputPath = $null
$ownsOutput = $false
$resultCode = 2

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 64), $utf8)
}
function File-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Is-Integer($Value) { ($Value -is [int]) -or ($Value -is [long]) }
function Is-Number($Value) { (Is-Integer $Value) -or ($Value -is [double]) -or ($Value -is [decimal]) }
function Assert-NoReparseAncestors([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Package/evidence paths cannot traverse reparse points.'
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

try {
    Assert-Condition ($env:OS -eq 'Windows_NT') 'This package requires native Windows 11 x64.'
    Assert-Condition ($Mode -cin @('describe', 'inventory', 'smoke', 'cases')) 'Mode must be describe, inventory, smoke or cases.'
    Assert-Condition ($TimeoutSeconds -ge 1 -and $TimeoutSeconds -le 1200) 'TimeoutSeconds must be 1..1200.'
    Assert-Condition ($LifecycleCycles -ge 0 -and $LifecycleCycles -le 100) 'LifecycleCycles must be 0..100.'
    Assert-Condition ($DeviceCycles -ge 1 -and $DeviceCycles -le 20) 'DeviceCycles must be 1..20.'
    Assert-Condition ($PresentSeconds -ge 0 -and $PresentSeconds -le 600) 'PresentSeconds must be 0..600.'
    if ($Mode -in @('smoke', 'cases')) {
        Assert-Condition ($Device -cmatch '^[0-9a-f]{32}$') 'Smoke/cases require an exact lowercase 32-hex Device UUID from inventory.'
    } else { Assert-Condition ($Device -eq '') 'Device is valid only for smoke/cases.' }
    if ($Mode -ne 'cases') {
        Assert-Condition ($LifecycleCycles -eq 2 -and $DeviceCycles -eq 1 -and $PresentSeconds -eq 0) 'Lifecycle options apply only to cases.'
    }
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Output)) 'Output must name a fresh evidence directory.'
    $packageRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $outputPath = [IO.Path]::GetFullPath($Output).TrimEnd('\')
    Assert-NoReparseAncestors $packageRoot
    Assert-NoReparseAncestors $outputPath
    Assert-Condition (-not $outputPath.StartsWith($packageRoot + '\', [StringComparison]::OrdinalIgnoreCase) -and
        $outputPath -ine $packageRoot) 'Evidence must be outside the immutable package directory.'
    Assert-Condition (-not (Test-Path -LiteralPath $outputPath)) 'Output already exists; evidence is never overwritten.'
    New-Item -ItemType Directory -Path $outputPath -ErrorAction Stop | Out-Null
    $ownsOutput = $true
    $resultCode = 1

    # CreateProcessW with an atomic job-list attribute closes the suspended-child
    # race as well as containing all descendants. Only the three pipe handles
    # are inherited. Closing this process's private job kills all owned children.
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Globalization;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

public static class SharkPackageNative {
    [StructLayout(LayoutKind.Sequential)] struct SA { public int length; public IntPtr descriptor; public int inherit; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct SI {
        public int cb; public string reserved, desktop, title;
        public uint x,y,xsize,ysize,xchars,ychars,fill,flags;
        public ushort show, reserved2; public IntPtr reservedPtr, input, output, error;
    }
    [StructLayout(LayoutKind.Sequential)] struct SIX { public SI si; public IntPtr attributes; }
    [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr process, thread; public uint pid, tid; }
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long processTime, jobTime; public uint flags;
        public UIntPtr minWorking, maxWorking; public uint processLimit;
        public UIntPtr affinity; public uint priority, scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct Limits {
        public BasicLimits basic;
        public ulong readOps,writeOps,otherOps,readBytes,writeBytes,otherBytes;
        public UIntPtr processMemory,jobMemory,peakProcess,peakJob;
    }
    [StructLayout(LayoutKind.Sequential)] struct Accounting {
        public long totalUser,totalKernel,periodUser,periodKernel;
        public uint faults,totalProcesses,activeProcesses,terminatedProcesses;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr sa, string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int kind,ref Limits value,int size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int kind,ref Accounting value,int size,IntPtr written);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateJobObject(IntPtr job,uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CreatePipe(out IntPtr read,out IntPtr write,ref SA sa,uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetHandleInformation(IntPtr handle,uint mask,uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileW(string name,uint access,uint share,ref SA sa,uint disposition,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,int flags,ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr kind,IntPtr value,IntPtr size,IntPtr previous,IntPtr returned);
    [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcessW(string app,StringBuilder command,IntPtr processSA,IntPtr threadSA,bool inherit,uint flags,IntPtr environment,string cwd,ref SIX startup,out PI info);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr process,out uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int kind,IntPtr buffer,int size,out int required);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid,uint index);
    static void Check(bool ok) { if(!ok) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    static void Close(ref IntPtr handle) { if(handle!=IntPtr.Zero && handle.ToInt64()!=-1) CloseHandle(handle); handle=IntPtr.Zero; }
    public sealed class Security { public bool elevated; public int integrity_rid; }
    public static Security ProcessSecurity() {
        IntPtr token=IntPtr.Zero, value=IntPtr.Zero;
        try {
            Check(OpenProcessToken(GetCurrentProcess(),8,out token));
            int required; value=Marshal.AllocHGlobal(4);
            Check(GetTokenInformation(token,20,value,4,out required));
            bool elevated=Marshal.ReadInt32(value)!=0; Marshal.FreeHGlobal(value); value=IntPtr.Zero;
            GetTokenInformation(token,25,IntPtr.Zero,0,out required);
            if(required<=0 || required>65536) throw new InvalidOperationException("Invalid token information size");
            value=Marshal.AllocHGlobal(required); Check(GetTokenInformation(token,25,value,required,out required));
            IntPtr sid=Marshal.ReadIntPtr(value); byte count=Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            if(count==0) throw new InvalidOperationException("Missing token integrity SID");
            return new Security { elevated=elevated, integrity_rid=Marshal.ReadInt32(GetSidSubAuthority(sid,(uint)count-1)) };
        } finally { if(value!=IntPtr.Zero) Marshal.FreeHGlobal(value); Close(ref token); }
    }
    public static string Quote(string arg) {
        if(arg==null || arg.IndexOf('\0')>=0) throw new ArgumentException("Invalid process argument");
        StringBuilder text=new StringBuilder("\""); int slashes=0;
        foreach(char c in arg) {
            if(c=='\\') { slashes++; continue; }
            if(c=='"') { text.Append('\\',slashes*2+1); text.Append(c); }
            else { text.Append('\\',slashes); text.Append(c); }
            slashes=0;
        }
        text.Append('\\',slashes*2); return text.Append('"').ToString();
    }
    sealed class Capture {
        public byte[] bytes; public string error; public int overflow;
        public void Read(IntPtr handle,int maximum) {
            try {
                using(FileStream stream=new FileStream(new SafeFileHandle(handle,true),FileAccess.Read,8192,false))
                using(MemoryStream retained=new MemoryStream()) {
                    byte[] block=new byte[8192]; int count;
                    while((count=stream.Read(block,0,block.Length))>0) {
                        int take=Math.Min(count,maximum-(int)retained.Length);
                        retained.Write(block,0,take);
                        if(take!=count) Interlocked.Exchange(ref overflow,1);
                    }
                    bytes=retained.ToArray();
                }
            } catch(Exception ex) { error=ex.Message; }
        }
        public string Text() { return bytes==null ? "" : new UTF8Encoding(false,true).GetString(bytes); }
    }
    public sealed class Result {
        public int exit_code; public uint pid; public bool timed_out,output_limit_exceeded,cleanup_complete;
        public double elapsed_seconds; public string stdout,stderr; public string[] cleanup_errors;
    }
    public static Result Run(string executable,string[] args,string cwd,IDictionary<string,string> environment,int timeout,int maximum) {
        IntPtr job=IntPtr.Zero, readOut=IntPtr.Zero,writeOut=IntPtr.Zero,readErr=IntPtr.Zero,writeErr=IntPtr.Zero,input=IntPtr.Zero;
        IntPtr attrs=IntPtr.Zero,jobList=IntPtr.Zero,handles=IntPtr.Zero,env=IntPtr.Zero;
        bool initialized=false; PI process=new PI(); Stopwatch clock=Stopwatch.StartNew();
        Capture stdout=new Capture(),stderr=new Capture(); Task outTask=null,errTask=null;
        List<string> errors=new List<string>(); Result result=new Result();
        try {
            job=CreateJobObjectW(IntPtr.Zero,null); Check(job!=IntPtr.Zero);
            Limits limits=new Limits(); limits.basic.flags=0x2000;
            Check(SetInformationJobObject(job,9,ref limits,Marshal.SizeOf(typeof(Limits))));
            SA sa=new SA { length=Marshal.SizeOf(typeof(SA)),inherit=1 };
            Check(CreatePipe(out readOut,out writeOut,ref sa,0)); Check(SetHandleInformation(readOut,1,0));
            Check(CreatePipe(out readErr,out writeErr,ref sa,0)); Check(SetHandleInformation(readErr,1,0));
            input=CreateFileW("NUL",0x80000000,3,ref sa,3,0,IntPtr.Zero); Check(input.ToInt64()!=-1);
            IntPtr required=IntPtr.Zero; InitializeProcThreadAttributeList(IntPtr.Zero,2,0,ref required);
            attrs=Marshal.AllocHGlobal(required); Check(InitializeProcThreadAttributeList(attrs,2,0,ref required)); initialized=true;
            jobList=Marshal.AllocHGlobal(IntPtr.Size); Marshal.WriteIntPtr(jobList,job);
            Check(UpdateProcThreadAttribute(attrs,0,new IntPtr(0x2000D),jobList,new IntPtr(IntPtr.Size),IntPtr.Zero,IntPtr.Zero));
            handles=Marshal.AllocHGlobal(IntPtr.Size*3);
            Marshal.WriteIntPtr(handles,0,input); Marshal.WriteIntPtr(handles,IntPtr.Size,writeOut); Marshal.WriteIntPtr(handles,IntPtr.Size*2,writeErr);
            Check(UpdateProcThreadAttribute(attrs,0,new IntPtr(0x20002),handles,new IntPtr(IntPtr.Size*3),IntPtr.Zero,IntPtr.Zero));
            SortedDictionary<string,string> sorted=new SortedDictionary<string,string>(environment,StringComparer.OrdinalIgnoreCase);
            StringBuilder block=new StringBuilder(); foreach(var pair in sorted) {
                if(pair.Key.IndexOf('\0')>=0 || pair.Value.IndexOf('\0')>=0) throw new ArgumentException("NUL in environment");
                block.Append(pair.Key).Append('=').Append(pair.Value).Append('\0');
            }
            block.Append('\0'); env=Marshal.StringToHGlobalUni(block.ToString());
            StringBuilder command=new StringBuilder(Quote(executable)); foreach(string arg in args) command.Append(' ').Append(Quote(arg));
            if(command.Length>=32767) throw new ArgumentException("Process command exceeds Windows limit");
            SIX startup=new SIX(); startup.si.cb=Marshal.SizeOf(typeof(SIX)); startup.si.flags=0x101;
            startup.si.show=0; startup.si.input=input; startup.si.output=writeOut; startup.si.error=writeErr; startup.attributes=attrs;
            Check(CreateProcessW(executable,command,IntPtr.Zero,IntPtr.Zero,true,0x08080404,env,cwd,ref startup,out process));
            result.pid=process.pid;
            Close(ref input); Close(ref writeOut); Close(ref writeErr);
            IntPtr ownedOut=readOut,ownedErr=readErr; readOut=readErr=IntPtr.Zero;
            outTask=Task.Factory.StartNew(()=>stdout.Read(ownedOut,maximum),TaskCreationOptions.LongRunning);
            errTask=Task.Factory.StartNew(()=>stderr.Read(ownedErr,maximum),TaskCreationOptions.LongRunning);
            Check(ResumeThread(process.thread)!=0xFFFFFFFF);
            while(true) {
                uint state=WaitForSingleObject(process.process,25);
                if(state==0) break;
                if(state!=0x102) throw new Win32Exception(Marshal.GetLastWin32Error());
                if(clock.Elapsed.TotalSeconds>=timeout) { result.timed_out=true; break; }
                if(Volatile.Read(ref stdout.overflow)!=0 || Volatile.Read(ref stderr.overflow)!=0) { result.output_limit_exceeded=true; break; }
            }
            if(result.timed_out || result.output_limit_exceeded) Check(TerminateJobObject(job,124));
            Check(WaitForSingleObject(process.process,5000)==0);
            uint code; Check(GetExitCodeProcess(process.process,out code)); result.exit_code=unchecked((int)code);
            // Terminate descendants even after the direct child exits successfully.
            Check(TerminateJobObject(job,124));
            Stopwatch cleanup=Stopwatch.StartNew();
            while(true) {
                Accounting state=new Accounting(); Check(QueryInformationJobObject(job,1,ref state,Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero));
                if(state.activeProcesses==0) { result.cleanup_complete=true; break; }
                if(cleanup.Elapsed.TotalSeconds>=5) { errors.Add("Owned job still has active processes after cleanup deadline"); break; }
                Thread.Sleep(10);
            }
            if(!Task.WaitAll(new Task[]{outTask,errTask},5000)) errors.Add("Captured streams did not close after owned-job cleanup");
            if(stdout.error!=null) errors.Add("stdout: "+stdout.error); if(stderr.error!=null) errors.Add("stderr: "+stderr.error);
            result.output_limit_exceeded |= stdout.overflow!=0 || stderr.overflow!=0;
            result.stdout=stdout.Text(); result.stderr=stderr.Text();
            if(result.timed_out) result.exit_code=124;
            else if(result.output_limit_exceeded || errors.Count!=0) result.exit_code=1;
            result.elapsed_seconds=clock.Elapsed.TotalSeconds; result.cleanup_errors=errors.ToArray(); return result;
        } finally {
            if(job!=IntPtr.Zero) TerminateJobObject(job,124);
            Close(ref job); Close(ref process.thread); Close(ref process.process);
            Close(ref readOut); Close(ref writeOut); Close(ref readErr); Close(ref writeErr); Close(ref input);
            if(initialized) DeleteProcThreadAttributeList(attrs);
            if(attrs!=IntPtr.Zero) Marshal.FreeHGlobal(attrs); if(jobList!=IntPtr.Zero) Marshal.FreeHGlobal(jobList);
            if(handles!=IntPtr.Zero) Marshal.FreeHGlobal(handles); if(env!=IntPtr.Zero) Marshal.FreeHGlobal(env);
        }
    }
    // PowerShell 5.1's JSON decoder accepts duplicate keys. Validate grammar,
    // duplicate keys, nesting and finite numbers before decoding a report.
    public static void ValidateJson(string text) { new JsonCheck(text).Run(); }
    sealed class JsonCheck {
        readonly string text; int pos;
        public JsonCheck(string value) { text=value; }
        void Bad() { throw new FormatException("Invalid, duplicate-key or nonfinite JSON at offset "+pos); }
        void White() { while(pos<text.Length && (text[pos]==' ' || text[pos]=='\t' || text[pos]=='\r' || text[pos]=='\n')) pos++; }
        bool Take(char c) { White(); if(pos<text.Length && text[pos]==c) { pos++; return true; } return false; }
        void Need(char c) { if(!Take(c)) Bad(); }
        public void Run() { Value(0); White(); if(pos!=text.Length) Bad(); }
        string String() {
            Need('"'); StringBuilder value=new StringBuilder();
            while(pos<text.Length) {
                char c=text[pos++]; if(c=='"') return value.ToString(); if(c<32) Bad();
                if(c=='\\') {
                    if(pos==text.Length) Bad(); c=text[pos++];
                    switch(c) {
                        case '"': case '\\': case '/': value.Append(c); break;
                        case 'b': value.Append('\b'); break; case 'f': value.Append('\f'); break;
                        case 'n': value.Append('\n'); break; case 'r': value.Append('\r'); break; case 't': value.Append('\t'); break;
                        case 'u':
                            if(pos+4>text.Length) Bad(); int scalar;
                            if(!Int32.TryParse(text.Substring(pos,4),NumberStyles.AllowHexSpecifier,CultureInfo.InvariantCulture,out scalar)) Bad();
                            value.Append((char)scalar); pos+=4; break;
                        default: Bad(); break;
                    }
                } else value.Append(c);
            }
            Bad(); return null;
        }
        void Value(int depth) {
            if(depth>64) Bad(); White(); if(pos==text.Length) Bad(); char c=text[pos];
            if(c=='{') {
                pos++; HashSet<string> keys=new HashSet<string>(StringComparer.Ordinal);
                if(Take('}')) return;
                do { string key=String(); if(!keys.Add(key)) Bad(); Need(':'); Value(depth+1); } while(Take(',')); Need('}');
            } else if(c=='[') { pos++; if(Take(']')) return; do { Value(depth+1); } while(Take(',')); Need(']'); }
            else if(c=='"') String();
            else if(c=='t' || c=='f' || c=='n') {
                string literal=c=='t'?"true":c=='f'?"false":"null";
                if(pos+literal.Length>text.Length || text.Substring(pos,literal.Length)!=literal) Bad(); pos+=literal.Length;
            } else {
                int start=pos; if(text[pos]=='-') pos++; if(pos==text.Length) Bad();
                if(text[pos]=='0') pos++;
                else { if(text[pos]<'1' || text[pos]>'9') Bad(); while(pos<text.Length && text[pos]>='0' && text[pos]<='9') pos++; }
                if(pos<text.Length && text[pos]=='.') { pos++; int first=pos; while(pos<text.Length && text[pos]>='0' && text[pos]<='9') pos++; if(pos==first) Bad(); }
                if(pos<text.Length && (text[pos]=='e' || text[pos]=='E')) { pos++; if(pos<text.Length && (text[pos]=='+' || text[pos]=='-')) pos++; int first=pos; while(pos<text.Length && text[pos]>='0' && text[pos]<='9') pos++; if(pos==first) Bad(); }
                double number; if(!Double.TryParse(text.Substring(start,pos-start),NumberStyles.Float,CultureInfo.InvariantCulture,out number) || Double.IsInfinity(number) || Double.IsNaN(number)) Bad();
            }
        }
    }
}
'@

    function Read-StrictJson([string]$Path, [long]$Maximum = 8388608) {
        $file = Get-Item -LiteralPath $Path
        Assert-Condition (-not $file.PSIsContainer -and $file.Length -le $Maximum) "JSON exceeds its file budget: $Path"
        $text = [IO.File]::ReadAllText($Path, $utf8)
        [SharkPackageNative]::ValidateJson($text)
        ConvertFrom-Json -InputObject $text
    }
    function Local-File([string]$Root, [string]$Relative) {
        Assert-NoReparseAncestors $Root
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($Relative) -and $Relative -notmatch '[\\:\x00]' -and
            -not $Relative.StartsWith('/') -and @($Relative.Split('/') | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -eq 0) 'Invalid package/artifact relative path.'
        $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
        Assert-Condition ($path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) 'Path escapes its owned root.'
        $item = Get-Item -LiteralPath $path
        Assert-Condition (-not $item.PSIsContainer) "Missing regular file: $Relative"
        while ($item.FullName -ine $Root) {
            Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Reparse points are not valid package/readback inputs.'
            $item = Get-Item -LiteralPath (Split-Path -Parent $item.FullName)
        }
        $path
    }
    function Check-Package($Manifest, [string]$Fingerprint) {
        Assert-Condition ((File-Hash $manifestPath) -ceq $Fingerprint) 'Package manifest changed during execution.'
        Assert-Condition ((Is-Integer $Manifest.schema_version) -and $Manifest.schema_version -eq 1 -and $Manifest.status -ceq 'PASS' -and $Manifest.qualification -ceq 'NOT_RUN') 'Unsupported or nonpassing package manifest.'
        Assert-Condition ((Is-Integer $Manifest.probe.schema_version) -and $Manifest.probe.schema_version -eq 2 -and $Manifest.probe.executable -ceq 'bin/GraphicsProbe.exe') 'Unsupported packaged probe identity.'
        foreach ($name in @('dependency_lock_sha256', 'probe_source_sha256')) {
            Assert-Condition ($Manifest.probe.$name -cmatch '^[0-9a-f]{64}$') 'Invalid compiled probe identity hash.'
        }
        $records = @($Manifest.files.PSObject.Properties)
        Assert-Condition ($records.Count -gt 0 -and $records.Count -le 512) 'Invalid package file count.'
        $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        [long]$total = 0
        foreach ($record in $records) {
            Assert-Condition ($names.Add($record.Name) -and $record.Name -ine 'package-manifest.json') 'Duplicate package path.'
            $file = Local-File $packageRoot $record.Name
            Assert-Condition ((Is-Integer $record.Value.bytes) -and $record.Value.bytes -ge 0 -and
                $record.Value.bytes -le 134217728 -and $record.Value.sha256 -cmatch '^[0-9a-f]{64}$') 'Invalid package file record.'
            $total += $record.Value.bytes
            Assert-Condition ($total -le 536870912 -and (Get-Item -LiteralPath $file).Length -eq $record.Value.bytes -and
                (File-Hash $file) -ceq $record.Value.sha256) "Package file identity mismatch: $($record.Name)"
        }
        foreach ($required in @('Run-GraphicsProbe.ps1', 'bin/GraphicsProbe.exe', 'bin/vulkan-1.dll',
            'shaders/fixture_vs.spv', 'shaders/fixture_ps.spv', 'shaders/compute_cs.spv', 'shaders/sample_ps.spv',
            'validation/VkLayer_khronos_validation.dll', 'validation/VkLayer_khronos_validation.json', 'validation/vk_layer_settings.txt')) {
            Assert-Condition ($names.Contains($required)) "Required package file absent: $required"
        }
        # Bounded traversal rejects unlisted DLLs/configuration and directory links.
        $pending = New-Object 'System.Collections.Generic.Stack[string]'
        $pending.Push($packageRoot); $visited = 0
        while ($pending.Count -gt 0) {
            foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                $visited++; Assert-Condition ($visited -le 1024) 'Package tree exceeds its entry budget.'
                Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Package contains a reparse point.'
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
                else {
                    $relative = $item.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
                    Assert-Condition ($relative -ceq 'package-manifest.json' -or $names.Contains($relative)) "Unlisted package file: $relative"
                }
            }
        }
        Assert-Condition ((Get-Item -LiteralPath (Join-Path $packageRoot 'validation/vk_layer_settings.txt')).Length -eq 0) 'Validation settings must remain empty.'
        $implicit = Join-Path $packageRoot 'validation/implicit'
        Assert-Condition ((Test-Path -LiteralPath $implicit -PathType Container) -and @(Get-ChildItem -LiteralPath $implicit -Force).Count -eq 0) 'Implicit layer directory must exist and remain empty.'
        $layer = Read-StrictJson (Join-Path $packageRoot 'validation/VkLayer_khronos_validation.json')
        Assert-Condition ($layer.layer.name -ceq 'VK_LAYER_KHRONOS_validation' -and
            $layer.layer.library_path -ceq '.\VkLayer_khronos_validation.dll') 'Packaged validation manifest must use the Windows-relative .\VkLayer_khronos_validation.dll path; the pinned loader does not resolve forward slashes or a bare name relative to this manifest.'
    }

    $manifestPath = Local-File $packageRoot 'package-manifest.json'
    $manifest = Read-StrictJson $manifestPath
    $fingerprint = File-Hash $manifestPath
    Check-Package $manifest $fingerprint
    $receipt.package_manifest_sha256 = $fingerprint
    $receipt.probe_identity = $manifest.probe
    $security = [SharkPackageNative]::ProcessSecurity()
    $receipt.process_security = $security
    if ($Mode -in @('smoke', 'cases') -and ($security.elevated -or $security.integrity_rid -lt 8192 -or $security.integrity_rid -ge 12288)) {
        $resultCode = 3
        throw 'Required validation needs a normal, non-administrator Windows terminal. Vulkan deliberately ignores local layer paths at high integrity; this package never changes identity or loader safeguards.'
    }

    $environment = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    $removed = @()
    $controlled = @('VK_INSTANCE_LAYERS', 'VK_LAYER_PATH', 'VK_ADD_LAYER_PATH', 'VK_LAYER_SETTINGS_PATH',
        'VK_LOADER_LAYERS_ENABLE', 'VK_LOADER_LAYERS_DISABLE', 'VK_LOADER_LAYERS_ALLOW',
        'VK_IMPLICIT_LAYER_PATH', 'VK_ADD_IMPLICIT_LAYER_PATH')
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $key = [string]$entry.Key
        if ($key -in $controlled -or $key.StartsWith('VK_LAYER_', [StringComparison]::OrdinalIgnoreCase) -or
            $key.StartsWith('VK_KHRONOS_VALIDATION_', [StringComparison]::OrdinalIgnoreCase)) { $removed += $key }
        else { $environment[$key] = [string]$entry.Value }
    }
    $applied = [ordered]@{
        VK_LAYER_PATH = (Join-Path $packageRoot 'validation')
        VK_LAYER_SETTINGS_PATH = (Join-Path $packageRoot 'validation/vk_layer_settings.txt')
        VK_IMPLICIT_LAYER_PATH = (Join-Path $packageRoot 'validation/implicit')
    }
    foreach ($key in $applied.Keys) { $environment[$key] = $applied[$key] }
    $drivers = [ordered]@{}
    foreach ($key in @('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES')) {
        if ($environment.ContainsKey($key)) { $drivers[$key] = $environment[$key] }
    }
    $receipt.environment = [ordered]@{ removed_ambient_keys = @($removed | Sort-Object); applied = $applied; driver_selection = $drivers }
    $executable = Join-Path $packageRoot 'bin/GraphicsProbe.exe'
    [string[]]$arguments = @('--' + $Mode)
    if ($Mode -in @('smoke', 'cases')) { $arguments += @('--device', $Device, '--validation', 'required') }
    $artifacts = Join-Path $outputPath 'artifacts'
    if ($Mode -eq 'cases') {
        $arguments += @('--shader-dir', (Join-Path $packageRoot 'shaders'), '--artifacts-dir', $artifacts,
            '--lifecycle-cycles', "$LifecycleCycles", '--device-cycles', "$DeviceCycles", '--present-seconds', "$PresentSeconds")
    }
    $receipt.command = @($executable) + $arguments
    $receipt.timeout_seconds = $TimeoutSeconds
    $process = [SharkPackageNative]::Run($executable, $arguments, $outputPath, $environment, $TimeoutSeconds, 8388608)
    [IO.File]::WriteAllText((Join-Path $outputPath 'stdout.json'), $process.stdout, $utf8)
    [IO.File]::WriteAllText((Join-Path $outputPath 'stderr.log'), $process.stderr, $utf8)
    $receipt.process = [ordered]@{
        pid = $process.pid; exit_code = $process.exit_code; elapsed_seconds = $process.elapsed_seconds
        timed_out = $process.timed_out; output_limit_exceeded = $process.output_limit_exceeded
        cleanup_complete = $process.cleanup_complete; cleanup_errors = @($process.cleanup_errors)
        containment = 'CreateProcessW atomic private job list, suspended start, restricted handle inheritance, kill-on-close'
        stdout_sha256 = (File-Hash (Join-Path $outputPath 'stdout.json'))
        stderr_sha256 = (File-Hash (Join-Path $outputPath 'stderr.log'))
    }
    Check-Package $manifest $fingerprint
    Assert-Condition ($process.cleanup_complete -and $process.cleanup_errors.Count -eq 0) 'Owned process-tree cleanup did not complete.'
    if ($process.timed_out) { $resultCode = 124; throw 'Probe exceeded its deadline; the owned process tree was terminated.' }
    Assert-Condition (-not $process.output_limit_exceeded) 'Probe output exceeded the 8 MiB per-stream budget.'
    $report = Read-StrictJson (Join-Path $outputPath 'stdout.json')
    $receipt.report = $report
    Assert-Condition ((Is-Integer $report.schema_version) -and $report.schema_version -eq 2 -and $report.mode -ceq $Mode -and
        $report.dependency_lock_sha256 -ceq $manifest.probe.dependency_lock_sha256 -and
        $report.probe_source_sha256 -ceq $manifest.probe.probe_source_sha256 -and
        $report.qualification -ceq 'NOT_RUN') 'Native report schema/mode/compiled identity/qualification mismatch.'
    Assert-Condition ((Is-Integer $report.exit_code) -and $report.exit_code -eq $process.exit_code) 'Native report exit code disagrees with process.'
    Assert-Condition ($process.exit_code -in @(0, 1, 2, 3)) 'Native probe returned an unsupported failure code.'
    $expectedStatus = if ($process.exit_code -eq 0) { 'PASS' } elseif ($process.exit_code -eq 3) { 'NOT_RUN' } else { 'FAIL' }
    Assert-Condition ($report.status -ceq $expectedStatus) 'Native report status disagrees with process.'
    $cases = @{}
    Assert-Condition (@($report.cases).Count -eq 8) 'Native report must retain all eight graphics cases.'
    foreach ($case in $report.cases) {
        Assert-Condition ($case.id -cmatch '^P02-0[1-8]$' -and -not $cases.ContainsKey($case.id) -and
            $case.status -cin @('PASS', 'FAIL', 'NOT_RUN')) 'Invalid or duplicate graphics case.'
        $cases[$case.id] = $case
        if ($Mode -ne 'cases' -or $case.id -in @('P02-07', 'P02-08')) {
            Assert-Condition ($case.status -ceq 'NOT_RUN') 'Single operation claimed an unexecuted or aggregate qualification.'
        }
    }
    if ($process.exit_code -ne 0) {
        # Failed comparisons may still contain useful readback bytes. Retain
        # their checked hashes without promoting any failed/unavailable case.
        $failedArtifacts = @(); $artifactNames = @{}; [long]$artifactTotal = 0
        $bytesPerPixel = @{ rgba8 = 4; 'rgba16f-le' = 8; 'd32f-le' = 4; 'u32-le' = 4 }
        foreach ($case in $report.cases) {
            if ($null -eq $case.PSObject.Properties['artifacts']) { continue }
            Assert-Condition ($Mode -eq 'cases' -and @($case.artifacts).Count -le 32) 'Unexpected or excessive failed-case artifacts.'
            foreach ($artifact in $case.artifacts) {
                Assert-Condition (-not $artifactNames.ContainsKey($artifact.path) -and $bytesPerPixel.ContainsKey($artifact.format)) 'Invalid failed-case artifact name/format.'
                $artifactNames[$artifact.path] = $true; [long]$bytes = $bytesPerPixel[$artifact.format]
                foreach ($field in @('width', 'height', 'layers')) {
                    Assert-Condition ((Is-Integer $artifact.$field) -and $artifact.$field -ge 1 -and $artifact.$field -le 16384) 'Invalid failed-case artifact dimensions.'
                    $bytes *= $artifact.$field
                }
                $artifactTotal += $bytes
                Assert-Condition ((Is-Integer $artifact.bytes) -and $artifact.bytes -eq $bytes -and $artifactTotal -le 67108864) 'Failed-case artifact byte budget mismatch.'
                $path = Local-File $artifacts $artifact.path
                Assert-Condition ((Get-Item -LiteralPath $path).Length -eq $bytes) 'Failed-case artifact file length mismatch.'
                $failedArtifacts += [ordered]@{ path = $artifact.path; bytes = $bytes; sha256 = (File-Hash $path) }
            }
        }
        $receipt.verified_artifacts = $failedArtifacts
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($report.error.code) -and
            -not [string]::IsNullOrWhiteSpace($report.error.message)) 'Native failure lacks an actionable structured error.'
        $resultCode = $process.exit_code
        throw ($report.error.code + ': ' + $report.error.message)
    }
    foreach ($field in @('validation_warnings', 'validation_errors')) {
        Assert-Condition ((Is-Integer $report.$field) -and $report.$field -eq 0) "Passing report has invalid/nonzero $field."
    }
    $stages = @()
    if ($Mode -ne 'describe') { $stages += @('loader', 'instance', 'surface', 'inventory') }
    if ($Mode -in @('smoke', 'cases')) { $stages += @('device', 'nvrhi_device_and_validation', 'swapchain') }
    if ($Mode -eq 'cases') { $stages += @('readback_cases', 'lifecycle') }
    if ($Mode -ne 'describe') { $stages += 'teardown' }
    Assert-Condition (@($report.stages).Count -eq $stages.Count) 'Native setup/teardown stages are incomplete.'
    for ($index = 0; $index -lt $stages.Count; $index++) {
        Assert-Condition ($report.stages[$index].name -ceq $stages[$index] -and $report.stages[$index].status -ceq 'PASS') 'Native setup/teardown stage failed or is out of order.'
    }
    $gpuOperation = $Mode -in @('smoke', 'cases')
    Assert-Condition (($report.vulkan_validation_enabled -is [bool]) -and $report.vulkan_validation_enabled -eq $gpuOperation -and
        ($report.nvrhi_validation_enabled -is [bool]) -and $report.nvrhi_validation_enabled -eq $gpuOperation) 'Required validation state mismatch.'
    Assert-Condition ($report.devices -is [array]) 'Native device inventory must be an array.'
    foreach ($item in $report.devices) { Assert-Condition ($item -is [pscustomobject]) 'Device inventory entries must be objects.' }
    if ($Mode -eq 'describe') {
        Assert-Condition (@($report.devices).Count -eq 0 -and $null -eq $report.selected_device_uuid) 'Description unexpectedly initialized a device.'
    } else { Assert-Condition (@($report.devices).Count -gt 0) 'Passing inventory requires an actual device.' }
    if ($gpuOperation) {
        $selected = @($report.devices | Where-Object { $_.uuid -ceq $Device })
        Assert-Condition ($selected.Count -eq 1 -and $report.selected_device_uuid -ceq $Device -and
            ($selected[0].reported_profile_supported -is [bool]) -and $selected[0].reported_profile_supported) 'Native probe did not uniquely select the requested capable device.'
        Assert-Condition ((Is-Integer $report.swapchain.image_count) -and $report.swapchain.image_count -ge 1 -and
            @($report.swapchain.extent).Count -eq 2 -and $report.swapchain.present_mode -ceq 'FIFO') 'Invalid swapchain evidence.'
        foreach ($dimension in $report.swapchain.extent) { Assert-Condition ((Is-Integer $dimension) -and $dimension -ge 1) 'Invalid swapchain extent.' }
        Assert-Condition ((Is-Integer $report.swapchain.presented_frames) -and $report.swapchain.presented_frames -ge 0 -and
            ($Mode -ne 'smoke' -or $report.swapchain.presented_frames -eq 0)) 'Invalid swapchain presented-frame evidence.'
    }
    if ($Mode -eq 'cases') {
        Assert-Condition (($report.validation_configuration.core -is [bool]) -and $report.validation_configuration.core -and
            ($report.validation_configuration.synchronization -is [bool]) -and $report.validation_configuration.synchronization -and
            ($report.validation_configuration.gpu_assisted -is [bool]) -and -not $report.validation_configuration.gpu_assisted -and
            @($report.validation_configuration.disabled_checks).Count -eq 0) 'Core/synchronization validation configuration mismatch.'
        foreach ($id in @('P02-01', 'P02-02', 'P02-03', 'P02-04', 'P02-06')) { Assert-Condition ($cases[$id].status -ceq 'PASS') "Required case $id did not pass." }
        $checks = @{
            'P02-02' = @{ textured_depth_winding_uv = @(12544, (1.0/255), 0); conventional_depth_readback = @(3136, 0.00001, 0) }
            'P02-03' = @{ integer_buffer_transform = @(256, 0, 0); rgba16f_storage_write = @(1024, 0.002, 0.002); compute_to_graphics_sample = @(1024, 0.002, 0.002) }
            'P02-04' = @{ hdr_attachment_blending = @(64, 0.002, 0.002); unorm_attachment_blending = @(64, (1.0/255), 0)
                srgb_unorm_transfer_roundtrip = @(32, 0, 0); srgb_decode_linear_filter = @(64, 0.002, 0.002)
                unorm_linear_filter = @(64, 0.002, 0.002); cube_six_faces_three_mips = @(1152, 0.002, 0.002)
                depth_array_twelve_layers_readback = @(768, 0.00001, 0); depth_array_twelve_layers_sample = @(768, 0.00001, 0)
                hdr_linear_filter = @(128, 0.002, 0.002) }
        }
        $expectedArtifacts = @{
            'P02-02' = @{ 'graphics.rgba8' = @('rgba8',64,64,1); 'graphics.d32' = @('d32f-le',64,64,1) }
            'P02-03' = @{ 'compute.u32' = @('u32-le',16,16,1); 'compute-storage.rgba16f' = @('rgba16f-le',16,16,1); 'compute-consumed.rgba16f' = @('rgba16f-le',16,16,1) }
            'P02-04' = @{ 'formats-hdr.rgba16f' = @('rgba16f-le',4,4,1); 'formats-unorm.rgba8' = @('rgba8',4,4,1)
                'formats-srgb-upload.rgba8' = @('rgba8',2,2,1); 'formats-linear-upload.rgba8' = @('rgba8',2,2,1)
                'formats-samples.rgba16f' = @('rgba16f-le',4,4,34); 'formats-depth-crops.d32' = @('d32f-le',8,8,12) }
        }
        $pixelBytes = @{ rgba8 = 4; 'rgba16f-le' = 8; 'd32f-le' = 4; 'u32-le' = 4 }
        $verified = @()
        foreach ($id in @('P02-02', 'P02-03', 'P02-04')) {
            $case = $cases[$id]
            Assert-Condition ($case.execution_status -ceq 'PASS' -and @($case.checks).Count -eq $checks[$id].Count) "Incomplete executed comparisons in $id."
            $seen = @{}
            foreach ($check in $case.checks) {
                Assert-Condition ($checks[$id].ContainsKey($check.name) -and -not $seen.ContainsKey($check.name)) 'Missing/duplicate/unknown readback comparison.'
                $seen[$check.name] = $true; $expected = $checks[$id][$check.name]
                Assert-Condition ($check.status -ceq 'PASS' -and (Is-Integer $check.compared_values) -and
                    $check.compared_values -ge $expected[0] -and (Is-Integer $check.failed_values) -and $check.failed_values -eq 0) 'Readback coverage decreased or comparison failed.'
                foreach ($field in @('max_absolute_error', 'absolute_tolerance', 'relative_tolerance', 'max_tolerance_ratio')) {
                    Assert-Condition ((Is-Number $check.$field) -and $check.$field -ge 0) 'Invalid measured readback number.'
                }
                Assert-Condition ($check.absolute_tolerance -le ($expected[1] + 1e-12) -and
                    $check.relative_tolerance -le $expected[2] -and $check.max_tolerance_ratio -le 1 -and
                    ($check.relative_tolerance -ne 0 -or $check.max_absolute_error -le $check.absolute_tolerance)) 'Readback exceeded or widened its fixed ADR tolerance.'
            }
            Assert-Condition (@($case.artifacts).Count -eq $expectedArtifacts[$id].Count) 'Readback artifact inventory is incomplete.'
            $seen = @{}
            foreach ($artifact in $case.artifacts) {
                Assert-Condition ($expectedArtifacts[$id].ContainsKey($artifact.path) -and -not $seen.ContainsKey($artifact.path)) 'Unexpected/duplicate readback artifact.'
                $seen[$artifact.path] = $true; $expected = $expectedArtifacts[$id][$artifact.path]
                Assert-Condition ($artifact.format -ceq $expected[0]) 'Readback format mismatch.'
                $index = 1
                foreach ($field in @('width', 'height', 'layers')) {
                    Assert-Condition ((Is-Integer $artifact.$field) -and $artifact.$field -eq $expected[$index]) 'Readback dimensions mismatch.'; $index++
                }
                $size = $expected[1] * $expected[2] * $expected[3] * $pixelBytes[$expected[0]]
                $path = Local-File $artifacts $artifact.path
                Assert-Condition ((Is-Integer $artifact.bytes) -and $artifact.bytes -eq $size -and
                    (Get-Item -LiteralPath $path).Length -eq $size) 'Readback byte count mismatch.'
                $verified += [ordered]@{ path = $artifact.path; bytes = $size; sha256 = (File-Hash $path) }
            }
        }
        $receipt.verified_artifacts = $verified
        $life = $report.lifecycle
        foreach ($pair in @(@('requested_resize_cycles',$LifecycleCycles),@('completed_resize_cycles',$LifecycleCycles),
            @('requested_device_cycles',$DeviceCycles),@('completed_device_cycles',$DeviceCycles),
            @('minimize_restore_cycles',$LifecycleCycles),@('requested_present_seconds',$PresentSeconds))) {
            Assert-Condition ((Is-Integer $life.($pair[0])) -and $life.($pair[0]) -eq $pair[1]) 'Requested/completed lifecycle evidence mismatch.'
        }
        foreach ($field in @('zero_extent_policy_checks','observed_zero_framebuffer_extents','presented_frames')) {
            Assert-Condition ((Is-Integer $life.$field) -and $life.$field -ge 0) 'Invalid measured lifecycle count.'
        }
        $extraFrame = if ($PresentSeconds -gt 0) { 1 } else { 0 }
        Assert-Condition ($life.observed_zero_framebuffer_extents -le $LifecycleCycles -and
            $life.presented_frames -ge (2*$LifecycleCycles + $DeviceCycles + $extraFrame) -and
            (Is-Number $life.present_elapsed_seconds) -and $life.present_elapsed_seconds -ge $PresentSeconds) 'Measured lifecycle workload is below the request.'
        $qualified = $LifecycleCycles -eq 100 -and $DeviceCycles -eq 20 -and $PresentSeconds -eq 600 -and $life.observed_zero_framebuffer_extents -gt 0
        $lifeStatus = if ($qualified) { 'PASS' } else { 'NOT_RUN' }
        Assert-Condition (($life.fully_qualified -is [bool]) -and $life.fully_qualified -eq $qualified -and
            $cases['P02-05'].status -ceq $lifeStatus) 'Lifecycle qualification disagrees with measured thresholds.'
    }
    $resultCode = 0
    $receipt.status = 'PASS'
} catch {
    $receipt.errors += $_.Exception.Message
    $receipt.status = if ($resultCode -eq 3) { 'NOT_RUN' } else { 'FAIL' }
} finally {
    $receipt.exit_code = $resultCode
    $receipt.finished_utc = [DateTime]::UtcNow.ToString('o')
    if ($ownsOutput) {
        # Never write into a destination rejected by the fresh-output preflight.
        if (-not (Test-Path -LiteralPath (Join-Path $outputPath 'summary.json'))) { Write-Json (Join-Path $outputPath 'summary.json') $receipt }
    }
    $receipt | ConvertTo-Json -Depth 64
}
exit $resultCode
