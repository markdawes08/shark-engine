"""CPU-only scene viewport oracle/evidence controls; these tests qualify no GPU."""
from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import scene_render_smoke as smoke
from process_runner import ProcessResult

LOCK, SOURCE, UUID = "a"*64, "b"*64, "c"*32


def adapter():
    return {"uuid":UUID,"name":"Synthetic CPU fixture","driver_name":"No GPU","driver_info":"No native libraries",
            "vendor_id":1,"device_id":2,"driver_version":3,"api_version":4,"queue_family":0,"kind":"discrete",
            "features":dict.fromkeys(smoke.foundation.FEATURES,True),"formats":dict.fromkeys(smoke.foundation.FORMATS,True),
            "limits":smoke.foundation.LIMITS.copy(),"missing_requirements":[]}


def identity(mode="render", fault="", validation="required"):
    return {"kind":"identity","schema_version":1,"assignment_13":"NOT_RUN","mode":mode,"dependency_lock_sha256":LOCK,
            "source_sha256":SOURCE,"compiler":"CPU fixture","selection":"" if mode=="describe" else UUID,"validation":validation,"fault":fault}


def viewport(case=0, *, frames=None, closed=False):
    width,height=(0,0) if closed else smoke.EXTENTS[case]
    generation=smoke.GENERATIONS[case]
    return {"extent":{"width":width,"height":height},"generation":generation,"retired_generations":generation if closed else generation-1,
            "rendered_frames":case+1 if frames is None else frames,"live_target_resources":0 if closed else 5,"pending":False}


def diagnostics(): return {"warnings":0,"errors":0,"dropped":0,"messages":[]}


def rows(validation="required"):
    status="PASS" if validation=="required" else "NOT_RUN"
    result=[identity(validation=validation),{"kind":"adapter","adapter":adapter()}]
    for case in range(5):
        result.append({"kind":"case",**smoke.fixture(case),"submitted":case+1,"completed":case+1,"created_resources":10,"retired_resources":10,
                       "subpixel_precision_bits":8,"viewport":viewport(case)})
    result.append({"kind":"complete","status":status,"qualification":status,"assignment_13":"NOT_RUN","cases":5,
                   "diagnostics":diagnostics(),"viewport":viewport(4,closed=True)})
    return result


def physics_rows():
    result = [identity("physics"), {"kind": "adapter", "adapter": adapter()}]
    for case in range(8):
        result.append({"kind": "physics-case", **smoke.physics_fixture(case),
                       "submitted": case+1, "completed": case+1,
                       "created_resources": 10, "retired_resources": 10,
                       "subpixel_precision_bits": 8,
                       "viewport": {"extent": {"width": 128, "height": 96},
                                    "generation": 1, "retired_generations": 0,
                                    "rendered_frames": case+1, "live_target_resources": 5,
                                    "pending": False}})
    result.append({"kind": "complete", "status": "PASS", "qualification": "PASS",
                   "assignment_18": "NOT_RUN", "cases": 8, "completed_ticks": 7,
                   "physics_closed": True, "diagnostics": diagnostics(),
                   "viewport": {"extent": {"width": 0, "height": 0}, "generation": 1,
                                "retired_generations": 1, "rendered_frames": 8,
                                "live_target_resources": 0, "pending": False}})
    return result


def process(records, code=0, stderr=""):
    return ProcessResult(["cpu-fixture"],code,"\n".join(json.dumps(row) for row in records),stderr,False)


def resources(target=0, pending=""):
    return {"target":target,"frame":10 if pending else 0,"submitted":int(pending=="submission-timeout"),
            "recording":int(pending=="recording-error"),"swapchains":0,"offscreen":0,"queue_workers":0}


def fault_rows(fault):
    error={"code":"execution_failed","operation":{"partial-creation":"graphics.test.scene_partial_creation","submission-timeout":"graphics.scene.wait_timeout","recording-error":"graphics.test.scene_recording_error"}[fault],"message":"Injected scene failure"}
    output=[identity("fault",fault),{"kind":"ready","adapter":adapter(),"resources":resources(5 if fault=="partial-creation" else 0),"viewport":viewport(),"diagnostics":diagnostics()},
            {"kind":"fault","fired":True,"error":error,"resources":resources(5,"" if fault=="partial-creation" else fault),"teardown_calls":0,"diagnostics":diagnostics()}]
    if fault=="recording-error":
        return output+[{"kind":"retained","error":{"code":"execution_failed","operation":"graphics.scene.drain","message":"Unresolved recording"},
                        "resources":resources(5,fault),"diagnostics":diagnostics(),"teardown_calls":0},
                       {"kind":"terminated","expected":True,"teardown_calls":0}]
    frames=2 if fault=="partial-creation" else 1
    return output+[{"kind":"retired","resources":resources(5),"diagnostics":diagnostics(),"teardown_calls":0},
                   {"kind":"recovered","submitted":3,"completed":3,"created_resources":10,"retired_resources":10,
                    "viewport":viewport(frames=frames),"center_rgba8":[0,255,255,128],"center_depth":.25,"diagnostics":diagnostics()},
                   {"kind":"closed","resources":resources(),"viewport":viewport(frames=frames,closed=True),"diagnostics":diagnostics(),"teardown_calls":5}]


class SceneViewportEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory()
        cls.output=Path(cls.temporary.name)
        for case in range(5):
            data,reference=smoke.fixture(case),smoke.reference(case)
            (cls.output/data["color_file"]).write_bytes(reference.color)
            (cls.output/data["depth_file"]).write_bytes(struct.pack(f"<{len(reference.depth)}f",*reference.depth))

    @classmethod
    def tearDownClass(cls): cls.temporary.cleanup()

    def inspect(self, records=None, validation="required", result=None):
        return smoke.inspect_result(result or process(records or rows(validation)),"render",UUID,validation,LOCK,SOURCE,self.output)

    def test_fixed_oracle_exercises_mirror_depth_and_empty_clear(self):
        for case in range(4):
            with self.subTest(case=case):
                reference=smoke.reference(case)
                self.assertGreater(reference.foreground,100)
                self.assertGreaterEqual(reference.occlusion,30)
                self.assertGreaterEqual(reference.background,100)
        ortho=smoke.reference(0)
        offset=65*96+48
        self.assertEqual(ortho.color[offset*4:offset*4+4],bytes((0,255,255,128)))
        self.assertEqual(ortho.depth[offset],.25)
        self.assertEqual(ortho.color,smoke.reference(3).color)
        self.assertEqual(smoke.reference(4).foreground,0)
        self.assertEqual(smoke.reference(4).background,96*80)
        self.assertFalse(self.inspect()[1])

    def test_device_free_description_and_validation_off_do_not_qualify(self):
        records=[identity("describe"),{"kind":"description","status":"PASS","qualification":"NOT_RUN","device_accessed":False,
                                     "cases":5,"faults":list(smoke.FAULTS),"native_graphics_available":False}]
        self.assertFalse(smoke.inspect_result(process(records),"describe","auto","required",LOCK,SOURCE)[1])
        self.assertFalse(self.inspect(validation="off")[1])
        records[1]["device_accessed"]=True
        self.assertTrue(smoke.inspect_result(process(records),"describe","auto","required",LOCK,SOURCE)[1])

    def test_identity_workload_retirement_and_diagnostics_rejections(self):
        controls=[(0,"assignment_13","PASS"),(0,"source_sha256","0"*64),(2,"draw_count",4),(3,"submitted",1),
                  (2,"completed",0),(2,"retired_resources",9),(2,"subpixel_precision_bits",4),(7,"cases",4)]
        for index,key,value in controls:
            with self.subTest(key=key):
                changed=rows(); changed[index][key]=value
                self.assertTrue(self.inspect(changed)[1])
        for index,key,value in ((2,"generation",2),(3,"generation",2),(4,"retired_generations",0),(7,"live_target_resources",5)):
            changed=rows(); changed[index]["viewport"][key]=value
            self.assertTrue(self.inspect(changed)[1])
        changed=rows(); changed[-1]["diagnostics"]["warnings"]=1
        self.assertTrue(self.inspect(changed)[1])

    def test_raw_geometry_and_depth_failures_are_detected(self):
        reference=smoke.reference(0)
        color=bytearray(reference.color); depth=list(reference.depth)
        pixel=next(index for index,ok in enumerate(reference.checked) if ok and reference.depth[index]<1)
        color[pixel*4]^=255
        packed=struct.pack(f"<{len(depth)}f",*depth)
        self.assertEqual(smoke.raster.compare_readback(color,packed,reference)["status"],"FAIL")
        depth[pixel]=1
        self.assertEqual(smoke.raster.compare_readback(reference.color,struct.pack(f"<{len(depth)}f",*depth),reference)["status"],"FAIL")

    def test_fault_traces_preserve_failures_and_retained_owners(self):
        for fault in smoke.FAULTS:
            with self.subTest(fault=fault):
                records=fault_rows(fault)
                fatal=fault=="recording-error"
                stderr=smoke.FATAL_PREFIX+records[3]["error"]["message"]+smoke.FATAL_SUFFIX if fatal else ""
                result=process(records,86 if fatal else 1,stderr)
                self.assertFalse(smoke.inspect_fault(result,fault,UUID,LOCK,SOURCE)[1])
                self.assertTrue(smoke.inspect_fault(replace(result,returncode=0),fault,UUID,LOCK,SOURCE)[1])
                changed=deepcopy(records); changed[2]["resources"]["target"]=0
                self.assertTrue(smoke.inspect_fault(process(changed,result.returncode,stderr),fault,UUID,LOCK,SOURCE)[1])
                changed=deepcopy(records); changed[3]["teardown_calls"]=False
                self.assertTrue(smoke.inspect_fault(process(changed,result.returncode,stderr),fault,UUID,LOCK,SOURCE)[1])

    def test_process_failure_and_partial_output_cannot_pass(self):
        base=process(rows())
        for changed in (replace(base,returncode=1),replace(base,timed_out=True),replace(base,stderr="unexpected"),
                        replace(base,cleanup_errors=("child alive",)),process(rows()[:-1]),replace(base,stdout="not json")):
            self.assertTrue(self.inspect(result=changed)[1])


class PhysicsPresentationEvidenceTests(unittest.TestCase):
    """Qualifies the evidence validator and literal oracle, never a GPU."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.output = Path(cls.temporary.name)
        for case in range(8):
            expected, reference = smoke.physics_fixture(case), smoke.physics_reference(case)
            (cls.output/expected["color_file"]).write_bytes(reference.color)
            (cls.output/expected["depth_file"]).write_bytes(
                struct.pack(f"<{len(reference.depth)}f", *reference.depth))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def inspect(self, records=None):
        return smoke.inspect_physics(process(records or physics_rows()), UUID, "required",
                                     LOCK, SOURCE, self.output)

    def test_literal_pose_camera_and_discontinuity_oracles(self):
        self.assertFalse(self.inspect()[1])
        # Alpha0 after the first step must retain the reset image, not final pose.
        self.assertEqual(smoke.physics_reference(1).color, smoke.physics_reference(2).color)
        self.assertNotEqual(smoke.physics_reference(2).color, smoke.physics_reference(3).color)
        self.assertNotEqual(smoke.physics_reference(3).color, smoke.physics_reference(4).color)
        self.assertNotEqual(smoke.physics_reference(5).color, smoke.physics_reference(6).color)
        self.assertEqual(smoke.physics_fixture(5)["positions"]["dynamic_child"][1], -2.)
        self.assertEqual(smoke.physics_fixture(6)["positions"]["dynamic_child"][1], 2.)
        for case in range(8):
            reference = smoke.physics_reference(case)
            self.assertGreaterEqual(reference.foreground, 60)
            self.assertGreaterEqual(reference.foreground + reference.background, 128*96*.95)

    def test_native_pose_identity_ownership_and_completion_cannot_be_fabricated(self):
        for index, key, value in ((0, "mode", "render"), (5, "camera_x", 0.),
                                  (5, "alpha", 1.), (7, "tick", 5),
                                  (2, "render_preserved_revision", False),
                                  (4, "completed", 0), (10, "physics_closed", False),
                                  (10, "assignment_18", "PASS")):
            with self.subTest(index=index, key=key):
                changed = physics_rows()
                changed[index][key] = value
                self.assertTrue(self.inspect(changed)[1])
        changed = physics_rows()
        changed[5]["positions"]["dynamic_child"][0] += .25
        self.assertTrue(self.inspect(changed)[1])
        changed = physics_rows()
        changed[-1]["diagnostics"]["warnings"] = 1
        self.assertTrue(self.inspect(changed)[1])
        changed = physics_rows()
        changed[2]["completed"] = True
        self.assertTrue(self.inspect(changed)[1])
        changed = physics_rows()
        changed[2]["created_resources"], changed[2]["retired_resources"] = 1, True
        self.assertTrue(self.inspect(changed)[1])

    def test_wrong_interpolation_camera_image_and_depth_fail_independent_pixels(self):
        expected, wrong = smoke.physics_reference(3), smoke.physics_reference(4)
        packed = struct.pack(f"<{len(wrong.depth)}f", *wrong.depth)
        self.assertEqual(smoke.raster.compare_readback(wrong.color, packed, expected)["status"], "FAIL")
        depths = list(expected.depth)
        pixel = next(index for index, ok in enumerate(expected.checked)
                     if ok and expected.depth[index] < 1)
        depths[pixel] = .5
        self.assertEqual(smoke.raster.compare_readback(expected.color,
                         struct.pack(f"<{len(depths)}f", *depths), expected)["status"], "FAIL")


def spawning_rows(validation="required"):
    result = [identity("spawning", validation=validation), {"kind": "adapter", "adapter": adapter()}]
    for case in range(smoke.SPAWNING_CASES):
        expected = smoke.spawning_fixture(case)
        result.append({"kind": "spawning-case", **expected,
                       "scene_instance": 100+expected["lifetime"],
                       "snapshot_revision": case % (smoke.SPAWNING_CYCLES*2)+2,
                       "submitted": case+1, "completed": case+1,
                       "created_resources": 10, "retired_resources": 10,
                       "subpixel_precision_bits": 8, "resources": resources(5),
                       "viewport": {"extent": {"width": 128, "height": 96},
                                    "generation": 1, "retired_generations": 0,
                                    "rendered_frames": case+1, "live_target_resources": 5,
                                    "pending": False}})
    status = "PASS" if validation == "required" else "NOT_RUN"
    result.append({"kind": "complete", "status": status, "qualification": status,
                   "assignment_21": "NOT_RUN", "cases": smoke.SPAWNING_CASES,
                   "closed_lifetimes": smoke.SPAWNING_LIFETIMES,
                   "unique_roots": smoke.SPAWNING_LIFETIMES*smoke.SPAWNING_CYCLES,
                   "teardown_calls": 5, "diagnostics": diagnostics(), "resources": resources(),
                   "viewport": {"extent": {"width": 0, "height": 0}, "generation": 1,
                                "retired_generations": 1, "rendered_frames": smoke.SPAWNING_CASES,
                                "live_target_resources": 0, "pending": False}})
    return result


class SpawnLifetimeEvidenceTests(unittest.TestCase):
    """Independent pixels and consumed report rejection controls; no GPU qualification."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.output = Path(cls.temporary.name)
        for case in range(smoke.SPAWNING_CASES):
            expected = smoke.spawning_fixture(case)
            reference = smoke.spawning_reference(expected["phase"] == "occupied")
            (cls.output/expected["color_file"]).write_bytes(reference.color)
            (cls.output/expected["depth_file"]).write_bytes(
                struct.pack(f"<{len(reference.depth)}f", *reference.depth))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def inspect(self, records=None, validation="required", result=None):
        return smoke.inspect_spawning(result or process(
            records if records is not None else spawning_rows(validation)), UUID, validation,
            LOCK, SOURCE, self.output)

    def test_literal_spawn_pixels_distinguish_live_geometry_and_retired_clear(self):
        self.assertFalse(self.inspect()[1])
        occupied, empty = smoke.spawning_reference(True), smoke.spawning_reference(False)
        self.assertGreaterEqual(occupied.foreground, 20)
        self.assertEqual(empty.foreground, 0)
        self.assertEqual(empty.background, 128*96)
        for actual, expected in ((occupied, empty), (empty, occupied)):
            self.assertEqual(smoke.raster.compare_readback(actual.color,
                struct.pack(f"<{len(actual.depth)}f", *actual.depth), expected)["status"], "FAIL")

    def test_capacity_generations_mutation_effects_and_gpu_retirement_are_required(self):
        for index, key, value in ((2, "created_bodies", 0), (3, "destroyed_scripts", 1),
                                  (3, "draw_count", 1), (2, "stale_rejected", False),
                                  (8, "owners_closed", False), (8, "initialized_scripts", 1),
                                  (2, "retired_resources", 9), (2, "completed", 0),
                                  (-1, "closed_lifetimes", 1), (-1, "unique_roots", 1)):
            with self.subTest(index=index, key=key):
                changed = spawning_rows()
                changed[index][key] = value
                self.assertTrue(self.inspect(changed)[1])
        changed = spawning_rows()
        changed[3]["resources"]["frame"] = 1
        self.assertTrue(self.inspect(changed)[1])
        changed = spawning_rows()
        changed[-1]["resources"]["target"] = 5
        self.assertTrue(self.inspect(changed)[1])
        changed = spawning_rows()
        for row in changed[2+smoke.SPAWNING_CYCLES*2:-1]:
            row["scene_instance"] = changed[2]["scene_instance"]
        self.assertTrue(self.inspect(changed)[1])
        changed = spawning_rows()
        changed[3]["snapshot_revision"] = changed[2]["snapshot_revision"]
        self.assertTrue(self.inspect(changed)[1])

    def test_partial_runs_diagnostics_and_validation_off_cannot_claim_qualification(self):
        self.assertFalse(self.inspect(validation="off")[1])
        changed = spawning_rows("off")
        changed[-1]["qualification"] = "PASS"
        self.assertTrue(self.inspect(changed, "off")[1])
        changed = spawning_rows()
        changed[-1]["diagnostics"]["warnings"] = 1
        self.assertTrue(self.inspect(changed)[1])
        changed = spawning_rows()
        changed[-1]["assignment_21"] = "PASS"
        self.assertTrue(self.inspect(changed)[1])
        self.assertTrue(self.inspect(spawning_rows()[:-1])[1])
        self.assertTrue(self.inspect(result=replace(process(spawning_rows()), timed_out=True))[1])


if __name__=="__main__": unittest.main()
