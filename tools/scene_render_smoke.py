"""Qualify the bounded scene viewport on one explicit GPU using independent readbacks."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import struct
import sys
import time

import graphics_device_smoke as foundation
import offscreen_smoke as raster
import offscreen_faults
import shader_pipeline
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from presentation_faults import FATAL_PREFIX, FATAL_SUFFIX, clean
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

NAMES = ("ortho", "perspective", "resized", "restored", "empty")
EXTENTS = ((96, 80), (96, 80), (128, 96), (96, 80), (96, 80))
GENERATIONS = (1, 1, 2, 3, 3)
FAULTS = ("partial-creation", "submission-timeout", "recording-error")
TIMEOUT_SECONDS = 45
require = raster.require
integer = raster.integer


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/scene_render_smoke.cpp"] = sha256(ROOT / "tests/scene_render_smoke.cpp")
    return dict(sorted(inputs.items()))


def fixture(case: int) -> dict:
    require(type(case) is int and 0 <= case < 5, "Unknown scene viewport case")
    width, height = EXTENTS[case]
    return {"name": NAMES[case], "extent": {"width": width, "height": height},
            "draw_count": 0 if case == 4 else 5, "color_file": NAMES[case] + ".rgba8",
            "depth_file": NAMES[case] + ".d32f"}


@lru_cache(maxsize=5)
def reference(case: int) -> raster.Reference:
    """Literal fixture geometry/projection; no engine matrices or child inputs are trusted."""
    width, height = EXTENTS[case]
    count = width * height
    rgba = bytearray((0, 0, 0, 255) * count)
    depth = [1.] * count
    checked = [True] * count
    layers = [[] for _ in range(count)]
    positions = ((-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5))
    gradient = ((1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1), (1, 1, 1, 1))
    # tx,ty,z,sx,sy,colors,factor,reversed,double-sided; hidden/frustum-rejected
    # instances are deliberately absent, while the single-sided backface remains.
    draws = (
        (-.85, 0, -2, 1.2, 1.2, gradient, (.5, 1, .25, 1), False, False),
        (-.85, 0, -4, 1.2, 1.2, ((.125, .25, 1, 1),) * 4, (1,) * 4, False, False),
        (.85, 0, -2, -1.2, 1.2, gradient, (.5, 1, .25, 1), False, False),
        (0, 1.25, -3, .8, .5, ((1, 1, 0, 1),) * 4, (1,) * 4, True, False),
        (0, -1.25, -3, .8, .5, ((0, 1, 1, .5),) * 4, (1,) * 4, True, True),
    ) if case != 4 else ()
    for tx, ty, z, sx, sy, colors, factor, reversed_winding, double_sided in draws:
        vertices = []
        for (x, y), color in zip(positions, colors, strict=True):
            x, y = tx + sx * x, ty + sy * y
            if case == 1:
                clip = (x / ((width / height) * -z), -y / -z, 9 / 8 + 9 / (8 * z), 1 / -z)
            else:
                clip = (x / ((width / height) * 2), -y / 2, (-z - 1) / 8, 1.)
            require(-1 <= clip[0] <= 1 and -1 <= clip[1] <= 1 and 0 <= clip[2] <= 1,
                    "Fixed scene reference requires no clipping")
            vertices.append(raster.snap_vertex(((clip[0] + 1) * width / 2,
                                                (clip[1] + 1) * height / 2, clip[2], clip[3], *color)))
        triangles = ((0, 2, 1), (0, 3, 2)) if reversed_winding else ((0, 1, 2), (0, 2, 3))
        for triangle in triangles:
            a, b, c = (vertices[index] for index in triangle)
            area = raster.edge(a, b, c)
            front = area > 0 if sx < 0 else area < 0
            if not front and not double_sided:
                continue
            lengths = [math.hypot(q[0]-p[0], q[1]-p[1]) for p, q in ((b,c),(c,a),(a,b))]
            for y in range(max(0, math.floor(min(a[1],b[1],c[1])-1)), min(height, math.ceil(max(a[1],b[1],c[1])+1))):
                for x in range(max(0, math.floor(min(a[0],b[0],c[0])-1)), min(width, math.ceil(max(a[0],b[0],c[0])+1))):
                    pixel = y * width + x
                    point = (x+.5,y+.5)
                    weights = [raster.edge(p,q,point)/area for p,q in ((b,c),(c,a),(a,b))]
                    distances = [weight*abs(area)/length for weight,length in zip(weights,lengths,strict=True)]
                    if min(distances) >= -1 and min(abs(value) for value in distances) <= 1:
                        checked[pixel] = False
                    if min(weights) < 0:
                        continue
                    z_value = sum(weight*vertex[2] for weight,vertex in zip(weights,(a,b,c),strict=True))
                    reciprocal_w = sum(weight*vertex[3] for weight,vertex in zip(weights,(a,b,c),strict=True))
                    color = [sum(weight*vertex[3]*vertex[channel+4] for weight,vertex in zip(weights,(a,b,c),strict=True)) / reciprocal_w * factor[channel] for channel in range(4)]
                    layers[pixel].append(z_value)
                    if z_value <= depth[pixel]:
                        depth[pixel] = z_value
                        rgba[pixel*4:pixel*4+4] = bytes(max(0,min(255,round(channel*255))) for channel in color)
    foreground = sum(ok and z < 1 for ok,z in zip(checked,depth,strict=True))
    background = sum(ok and z == 1 for ok,z in zip(checked,depth,strict=True))
    occlusion = sum(checked[index] and any(z > depth[index]+1e-4 for z in samples) for index,samples in enumerate(layers))
    require(foreground+background >= math.ceil(count*.65), "Reference edge exclusions exceed fixed budget")
    if case != 4:
        require(foreground >= count*.08 and occlusion >= 30, "Reference lacks foreground/depth overlap")
    return raster.Reference(width,height,bytes(rgba),tuple(depth),tuple(checked),foreground,background,occlusion)


def rows_from(result: ProcessResult) -> list[dict]:
    require(not result.timed_out and not result.cleanup_errors, "Child deadline or process cleanup failure")
    require(len(result.stdout.encode("utf-8")) <= 1024*1024, "Native output exceeds byte budget")
    rows = [json.loads(line,object_pairs_hook=unique_object,parse_constant=reject_constant,parse_float=finite_float)
            for line in result.stdout.splitlines()]
    require(all(isinstance(row,dict) for row in rows), "Native rows must be objects")
    return rows


def check_identity(row: dict, mode: str, device: str, validation: str, lock: str, digest: str, fault: str="") -> None:
    expected = {"kind":"identity","schema_version":1,"assignment_13":"NOT_RUN","mode":mode,
                "dependency_lock_sha256":lock,"source_sha256":digest,"selection":device,
                "validation":validation,"fault":fault}
    require(set(row)=={*expected,"compiler"} and raster.close_values({key:row[key] for key in expected},expected)
            and isinstance(row["compiler"],str) and bool(row["compiler"]), "Native identity does not match inputs")


def check_viewport(value: object, extent: tuple[int,int], generation: int, frames: int, closed: bool=False) -> None:
    expected = {"extent":dict(zip(("width","height"),(0,0) if closed else extent)),"generation":generation,
                "retired_generations":generation if closed else generation-1,"rendered_frames":frames,
                "live_target_resources":0 if closed else 5,"pending":False}
    require(raster.close_values(value,expected), "Viewport generation, target ownership or completion differs")


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str, lock: str,
                   digest: str, readbacks: Path | None=None) -> tuple[list[dict],list[str],list[dict]]:
    rows, errors, comparisons = [], [], []
    try:
        require(result.returncode==0 and not result.stderr, "Scene fixture failed or emitted diagnostics")
        rows=rows_from(result)
        require(len(rows)==(2 if mode=="describe" else 8), "Wrong scene case count")
        check_identity(rows[0],mode,"" if mode=="describe" else device,validation,lock,digest)
        if mode=="describe":
            expected={"kind":"description","status":"PASS","qualification":"NOT_RUN","device_accessed":False,"cases":5,"faults":list(FAULTS)}
            require(set(rows[1])=={*expected,"native_graphics_available"} and type(rows[1]["native_graphics_available"]) is bool and
                    raster.close_values({key:rows[1][key] for key in expected},expected), "Description accessed graphics or changed workload")
            return rows,errors,comparisons
        require(mode=="render" and readbacks is not None, "Native comparison needs readback directory")
        require(set(rows[1])=={"kind","adapter"} and rows[1]["kind"]=="adapter", "Missing adapter row")
        require(not foundation.adapter_errors(rows[1]["adapter"],require_supported=True) and rows[1]["adapter"]["uuid"]==device,"Wrong or unsupported selected device")
        previous=0
        expected_files=set()
        for case,row in enumerate(rows[2:7]):
            expected=fixture(case)
            require(set(row)=={*expected,"kind","submitted","completed","created_resources","retired_resources","subpixel_precision_bits","viewport"} and row["kind"]=="case" and
                    raster.close_values({key:row[key] for key in expected},expected), "Native fixture inputs differ")
            require(integer(row["submitted"]) and row["submitted"]>previous and integer(row["completed"]) and row["completed"]==row["submitted"], "Readback lacks completed submission")
            previous=row["submitted"]
            require(integer(row["created_resources"]) and row["created_resources"]>0 and integer(row["retired_resources"]) and row["created_resources"]==row["retired_resources"], "Transient resources remain live")
            require(type(row["subpixel_precision_bits"]) is int and row["subpixel_precision_bits"]==8,"Reference requires current-PC8bit subpixel profile")
            check_viewport(row["viewport"],EXTENTS[case],GENERATIONS[case],case+1)
            paths=[shader_pipeline.contained(readbacks,expected[field]) for field in ("color_file","depth_file")]
            for path in paths:
                expected_files.add(path.name)
                require(path.is_file() and path.stat().st_size==EXTENTS[case][0]*EXTENTS[case][1]*4,"Wrong readback extent/format")
            comparison=raster.compare_readback(paths[0].read_bytes(),paths[1].read_bytes(),reference(case))
            comparison.update(name=expected["name"],color_sha256=sha256(paths[0]),depth_sha256=sha256(paths[1]))
            comparisons.append(comparison)
            if comparison["status"]!="PASS": errors.append(expected["name"]+" failed fixed color/depth tolerances")
        require({path.name for path in readbacks.iterdir()}==expected_files,"Unexpected readback files")
        complete=rows[-1]
        status="PASS" if validation=="required" else "NOT_RUN"
        expected={"kind":"complete","status":status,"qualification":status,"assignment_13":"NOT_RUN","cases":5}
        require(set(complete)=={*expected,"diagnostics","viewport"} and raster.close_values({key:complete[key] for key in expected},expected),"Wrong completion state")
        clean(complete["diagnostics"])
        check_viewport(complete["viewport"],EXTENTS[-1],3,5,True)
    except (OSError,ValueError,TypeError,KeyError,OverflowError,RecursionError) as error:
        errors.append(str(error))
    return rows,errors,comparisons


def resource_state(value: object, target: int, *, pending: str="") -> None:
    fields={"target","frame","submitted","recording","swapchains","offscreen","queue_workers"}
    require(isinstance(value,dict) and set(value)==fields and all(integer(item) for item in value.values()),"Invalid native resource counters")
    require(value["target"]==target and value["swapchains"]==value["offscreen"]==value["queue_workers"]==0 and
            (value["frame"]>0 if pending else value["frame"]==0) and value["submitted"]==int(pending=="submission-timeout") and
            value["recording"]==int(pending=="recording-error"),"Retained native owners differ from fault state")


def inspect_fault(result: ProcessResult, fault: str, device: str, lock: str, digest: str) -> tuple[list[dict],list[str]]:
    rows,errors=[],[]
    try:
        fatal=fault=="recording-error"
        require(fault in FAULTS and result.returncode==(86 if fatal else 1),"Fault lost expected nonzero exit")
        rows=rows_from(result)
        require([row.get("kind") for row in rows]==(["identity","ready","fault","retained","terminated"] if fatal else ["identity","ready","fault","retired","recovered","closed"]),"Incomplete fault lifecycle")
        check_identity(rows[0],"fault",device,"required",lock,digest,fault)
        require(not foundation.adapter_errors(rows[1]["adapter"],require_supported=True) and rows[1]["adapter"]["uuid"]==device,"Wrong fault adapter")
        resource_state(rows[1]["resources"],5 if fault=="partial-creation" else 0)
        if fault=="partial-creation": check_viewport(rows[1]["viewport"],EXTENTS[0],1,1)
        observed=rows[2]
        require(observed.get("fired") is True and type(observed.get("teardown_calls")) is int and observed["teardown_calls"]==0,"Fault missing or teardown premature")
        operation={"partial-creation":"graphics.test.scene_partial_creation","submission-timeout":"graphics.scene.wait_timeout","recording-error":"graphics.test.scene_recording_error"}[fault]
        offscreen_faults.owned_error(observed["error"],operation)
        resource_state(observed["resources"],5,pending="" if fault=="partial-creation" else fault)
        if fatal:
            retained=rows[3]
            resource_state(retained["resources"],5,pending=fault)
            offscreen_faults.owned_error(retained["error"],"graphics.scene.drain")
            require(type(retained["teardown_calls"]) is int and retained["teardown_calls"]==0 and raster.close_values(rows[4],{"kind":"terminated","expected":True,"teardown_calls":0}),"Fatal path released unresolved owners")
            require(result.stderr==FATAL_PREFIX+retained["error"]["message"]+FATAL_SUFFIX,"Wrong fatal diagnostic")
            checked=rows[1:4]
        else:
            require(not result.stderr,"Recovered fault emitted stderr")
            resource_state(rows[3]["resources"],5)
            require(type(rows[3]["teardown_calls"]) is int and rows[3]["teardown_calls"]==0,"Drain destroyed the device")
            recovered=rows[4]
            require(all(integer(recovered[key]) for key in ("submitted","completed","created_resources","retired_resources")) and recovered["submitted"]>0 and recovered["completed"]==recovered["submitted"] and
                    recovered["created_resources"]>0 and recovered["retired_resources"]==recovered["created_resources"],"Recovery lacks completed GPU work")
            require(isinstance(recovered["center_rgba8"],list) and len(recovered["center_rgba8"])==4 and all(integer(channel,255) for channel in recovered["center_rgba8"]) and recovered["center_rgba8"][:3]==[0,255,255] and abs(recovered["center_rgba8"][3]-128)<=1 and type(recovered["center_depth"]) in (int,float) and math.isfinite(recovered["center_depth"]) and abs(recovered["center_depth"]-.25)<=1e-5,"Recovery known pixel mismatch")
            check_viewport(recovered["viewport"],EXTENTS[0],1,2 if fault=="partial-creation" else 1)
            resource_state(rows[5]["resources"],0)
            check_viewport(rows[5]["viewport"],EXTENTS[0],1,2 if fault=="partial-creation" else 1,True)
            require(type(rows[5]["teardown_calls"]) is int and rows[5]["teardown_calls"]==5,"Recovered device teardown incomplete")
            checked=rows[1:]
        for row in checked: clean(row["diagnostics"])
    except (ValueError,TypeError,KeyError,IndexError,OverflowError,RecursionError) as error:
        errors.append(str(error))
    return rows,errors


def shader_inputs(root: Path) -> tuple[dict,Path,Path]:
    manifest=shader_pipeline.verify(root)
    recipe=shader_pipeline.recipe(ROOT/"shaders/scene_recipe.json")
    require(manifest["inputs"]["recipe"]==recipe and manifest["inputs"]["toolchain"]["lock_sha256"]==sha256(ROOT/"dependencies.lock.json") and
            manifest["inputs"]["pipeline_sha256"]==sha256(ROOT/"tools/shader_pipeline.py"),"Stale scene shader recipe/toolchain")
    sources=shader_pipeline.source_closure(ROOT/"shaders",recipe)
    require(manifest["inputs"]["sources"]=={name:{"sha256":shader_pipeline.digest(data),"bytes":len(data)} for name,data in sources.items()},"Stale scene shader sources")
    rows={row["id"]:row for row in manifest["shaders"]}
    require(set(rows)=={"scene-unlit-vs","scene-unlit-ps"},"Wrong scene shader pair")
    return manifest,shader_pipeline.contained(root,rows["scene-unlit-vs"]["file"]),shader_pipeline.contained(root,rows["scene-unlit-ps"]["file"])


def cli_checks(executable: Path) -> int:
    errors=[]
    lock,digest=sha256(ROOT/"dependencies.lock.json"),foundation.source_digest(source_inputs())
    for flag in ("--describe","--help"):
        result=run_process([str(executable),flag],cwd=executable.parent,timeout=5)
        errors.extend(inspect_result(result,"describe","auto","required",lock,digest)[1])
    for arguments in ([],["--render"],["--describe","extra"],["--native-fault","unknown","--device","0"*32,"--vertex","x","--pixel","y"]):
        result=run_process([str(executable),*arguments],cwd=executable.parent,timeout=5)
        try:
            rows=rows_from(result)
            require(result.returncode!=0 and not result.stderr and len(rows)==1 and rows[0]["error"]["operation"]=="scene-render-smoke.arguments","Malformed CLI did not reject before GPU access")
        except (ValueError,KeyError) as error: errors.append(str(error))
    print(json.dumps({"status":"FAIL" if errors else "PASS","device_accessed":False,"errors":errors}))
    return int(bool(errors))


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable",type=Path,required=True)
    parser.add_argument("--mode",choices=("describe","render","faults"),default="describe")
    parser.add_argument("--cli-only",action="store_true")
    parser.add_argument("--device",default="auto")
    parser.add_argument("--shaders",type=Path)
    parser.add_argument("--validation",choices=("required","off"),default="required")
    parser.add_argument("--validation-manifest",type=Path)
    parser.add_argument("--build-dir",type=Path)
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    executable=args.executable.resolve()
    if not executable.is_file(): parser.error("Requires built SceneRenderSmoke executable")
    if args.cli_only:
        if args.mode!="describe" or args.device!="auto" or args.shaders or args.output or args.validation_manifest or args.validation!="required": parser.error("CLI-only accepts no native inputs")
        return cli_checks(executable)
    if sys.platform!="win32" or args.output is None: parser.error("Requires Windows and fresh --output")
    if args.mode!="describe" and (not re.fullmatch(r"[0-9a-f]{32}",args.device) or args.shaders is None): parser.error("Native mode needs explicit UUID and verified --shaders")
    if args.mode=="describe" and (args.device!="auto" or args.shaders or args.validation!="required"): parser.error("Description uses no GPU inputs")
    if args.mode=="faults" and args.validation!="required": parser.error("Fault qualification requires validation")
    output=args.output.absolute()
    try:
        shader_pipeline.no_links(output); output.mkdir(parents=True,exist_ok=False)
    except (OSError,ValueError) as error: parser.error(str(error))
    summary={"schema_version":1,"checkpoint":"13d","assignment_13":"NOT_RUN","status":"FAIL","qualification":"NOT_RUN","mode":args.mode,
             "started_utc":datetime.now(timezone.utc).isoformat(),"timeout_seconds_per_child":TIMEOUT_SECONDS,"runs":[],"errors":[]}
    started=time.monotonic(); exit_code=1
    try:
        lock=sha256(ROOT/"dependencies.lock.json"); inputs=source_inputs(); digest=foundation.source_digest(inputs)
        summary.update(dependency_lock_sha256=lock,source_sha256=digest,input_sha256=inputs,executable_sha256=sha256(executable),
                       git_revision=command_output(["git","rev-parse","HEAD"]),git_status=command_output(["git","status","--short"]))
        tool_names=("scene_render_smoke.py","offscreen_smoke.py","offscreen_faults.py","presentation_faults.py","shader_pipeline.py","graphics_device_smoke.py","graphics_probe.py","process_runner.py","toolchain_report.py","windows_unelevated.py")
        summary["tool_inputs"]={name:sha256(ROOT/"tools"/name) for name in tool_names}
        manifest=None; validation=None
        if args.mode!="describe":
            manifest,vertex,pixel=shader_inputs(args.shaders.absolute()); summary["shader_manifest"]=manifest
            if args.validation=="required":
                if args.validation_manifest is None: raise Unavailable("Required validation needs locked --validation-manifest")
                validation=validation_tools(args.validation_manifest.resolve(),lock)
                summary["validation_tools"]={"manifest_path":str(args.validation_manifest.resolve()),"manifest_sha256":sha256(args.validation_manifest),"receipt":validation}
                summary["process_security"]=check_layer_process()
        settings=output/"vk_layer_settings.txt"; settings.write_text("# Required core/synchronization and NVRHI validation; no overrides.\n",encoding="utf-8")
        implicit=output/"implicit-layers-empty"; implicit.mkdir()
        environment,summary["layer_environment"]=layer_environment(validation,settings,implicit)
        (output/"environment.json").write_text(json.dumps(collect(args.build_dir.resolve() if args.build_dir else None),indent=2)+"\n",encoding="utf-8")
        modes=FAULTS if args.mode=="faults" else (args.mode,)
        for mode in modes:
            command=[str(executable),"--native-fault",mode] if args.mode=="faults" else [str(executable),"--"+mode]
            if args.mode!="describe": command += ["--device",args.device,"--vertex",str(vertex),"--pixel",str(pixel)]
            if args.mode=="render": command += ["--output",str(output/"readbacks"),"--validation",args.validation]
            result=run_process(command,cwd=executable.parent,timeout=TIMEOUT_SECONDS,env=environment)
            (output/(mode+".jsonl")).write_text(result.stdout,encoding="utf-8"); (output/(mode+".stderr.log")).write_text(result.stderr,encoding="utf-8")
            comparisons=[]
            if args.mode=="faults": rows,errors=inspect_fault(result,mode,args.device,lock,digest)
            else: rows,errors,comparisons=inspect_result(result,mode,args.device,args.validation,lock,digest,output/"readbacks")
            summary["runs"].append({"mode":mode,"command":command,"process_exit_code":result.returncode,"timed_out":result.timed_out,"cleanup_errors":list(result.cleanup_errors),"records":rows,"comparisons":comparisons,"errors":errors})
            summary["errors"].extend(errors)
            if args.mode=="render":
                for case in range(len(comparisons)):
                    raw=(output/"readbacks"/fixture(case)["color_file"]).read_bytes()
                    raster.write_png(output/(NAMES[case]+".png"),*EXTENTS[case],raw)
                    raster.write_png(output/(NAMES[case]+"-reference.png"),*EXTENTS[case],reference(case).color)
        require(source_inputs()==inputs and sha256(executable)==summary["executable_sha256"],"Compiled inputs changed during run")
        require({name:sha256(ROOT/"tools"/name) for name in tool_names}==summary["tool_inputs"],"Verification tools changed during run")
        if validation is not None: require(validation_tools(args.validation_manifest.resolve(),lock)==validation,"Validation tools changed during run")
        if manifest is not None: require(shader_inputs(args.shaders.absolute())[0]==manifest,"Shaders changed during run")
        if not summary["errors"]:
            summary["status"]="NOT_RUN" if args.validation=="off" else "PASS"
            if args.mode!="describe" and args.validation=="required": summary["qualification"]="PASS"
            exit_code=0
    except Unavailable as error: summary.update(status="NOT_RUN",errors=[str(error)]); exit_code=3
    except (OSError,ValueError,TypeError,KeyError) as error: summary["errors"].append(str(error))
    finally:
        summary.update(exit_code=exit_code,elapsed_seconds=round(time.monotonic()-started,3),finished_utc=datetime.now(timezone.utc).isoformat())
        (output/"summary.json").write_text(json.dumps(summary,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"status":summary["status"],"qualification":summary["qualification"],"exit_code":exit_code,"summary":str(output/"summary.json")}))
    return exit_code

if __name__=="__main__": raise SystemExit(main())
