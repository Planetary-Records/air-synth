"""Portable observer checks; synthetic packets never establish native acceptance."""
import copy
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
SPEC = importlib.util.find_spec('saturation_serial_observer')
observer = importlib.util.module_from_spec(SPEC) if SPEC else None
if SPEC:
    SPEC.loader.exec_module(observer)


class SerialObserverTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(observer, 'serial observer capability must exist')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def test_audio_qualification(self):
        raw = struct.pack('<4096d', *([0.0] * 4096))
        self.assertEqual(observer.qualify_audio(raw, raw)['frames'], 2048)
        changed = bytearray(raw); changed[0] = 1
        with self.assertRaises(ValueError):
            observer.qualify_audio(bytes(changed), raw)
        for data in (raw[:-8], struct.pack('<d', float('inf')) + raw[8:]):
            with self.assertRaises(ValueError):
                observer.qualify_audio(data, data)

    def test_canonical_gain_delegation_observes_actual_argument_and_result(self):
        original=(ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes().replace(
            b'return std::pow(10., db / 20);',b'return gain_math::analyticGain(db);',1)
        generated=observer.generate_candidate(original)
        self.assertEqual(observer.recover(generated),original)
        self.assertIn(b'serial_observer::gainResult(50, 0, db, ',generated)
        self.assertIn(b'gain_math::analyticGain(db)',generated)

    def test_candidate_and_original_serial_body_recovery(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        generated = observer.generate_candidate(original)
        self.assertNotEqual(original, generated)
        self.assertEqual(observer.recover(generated), original)
        fixture = (ROOT/'probes/saturation/contracts/SaturationFixtureTests.cpp').read_bytes()
        driver = observer.generate_driver(fixture)
        self.assertEqual(observer.recover(driver), fixture)
        self.assertIn(b'serial_observer::bank', driver)
        self.assertIn(b'runSerial(c, partition)', driver)
        self.assertIn(b'serial_observer::laneQuotient', generated)
        self.assertIn(b'serial_observer::gainResult(50', generated)
        self.assertIn(b'serial_observer::gainResult(51', generated)
        self.assertIn(b'serial_observer::state(*state)', generated)
        self.assertNotIn(b'#define AIRACK_SATURATION_NUMERICAL_TEST', generated)
        self.assertNotIn(b'#define AIRACK_SATURATION_TRANSITIONS', generated)

    def test_indexed_lane_point_is_actual_and_anchor_fails_closed(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        anchor = b'            auto& frame = frames[n];'
        generated = observer.generate_candidate(original)
        self.assertEqual(observer.recover(generated), original)
        lane = generated[generated.index(b'    void process4Character('):generated.index(b'    void process4(')]
        self.assertLess(lane.index(anchor), lane.index(b'serial_observer::lanePoint(this, int(&frame - frames.data()));'))
        self.assertLess(lane.index(b'serial_observer::lanePoint(this, int(&frame - frames.data()));'), lane.index(b'            const auto x = frame;'))
        for replacement in (b'', anchor + b'\n' + anchor, anchor.replace(b' ', b'\t', 1)):
            with self.subTest(edit=replacement), self.assertRaises(ValueError):
                observer.generate_candidate(original.replace(anchor, replacement, 1))

    def test_duplicate_missing_and_malformed_anchors_refused(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        anchor = b'#include <vector>'
        for bad in (original.replace(anchor, b''), original + anchor):
            with self.assertRaises(ValueError):
                observer.generate_candidate(bad)
        generated = observer.generate_candidate(original)
        with self.assertRaises(ValueError):
            observer.recover(generated.replace(observer.END, b'', 1))
        fixture = (ROOT/'probes/saturation/contracts/SaturationFixtureTests.cpp').read_bytes()
        for bad in (fixture.replace(b'std::array<Candidate, 8> bank;', b''),
                    fixture + b'std::array<Candidate, 8> bank;'):
            with self.assertRaises(ValueError):
                observer.generate_driver(bad)

    def test_pre_observation_follows_trim_and_filter_before_upsampling(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        generated = observer.generate_candidate(original)
        prepared = generated[generated.index(b'    Frame processPrepared('):]
        anchors = (b's.advanceControls();', b'serial_observer::bind(s); serial_observer::state(s);',
                   b'Frame wetInput;', b'input[0] *= s.inputGain; input[1] *= s.inputGain;',
                   b'wetInput = input;', b'if (s.current.preLowCutEnabled) wetInput = s.preCut.process<ProductState>(input, s.pre);',
                   b'serial_observer::pre(input, wetInput);', b's.wetUp.up4(wetInput,')
        positions = [prepared.index(anchor) for anchor in anchors]
        self.assertEqual(positions, sorted(positions))
        control_end = prepared.index(b'        std::array<Frame, Capacity> wet, dry, output;')
        bind = prepared.index(b'serial_observer::bind(s); serial_observer::state(s);')
        for anchor in (b's.advanceControls();', b's.advanceControlsFrom(*common);'):
            self.assertLess(prepared.rindex(anchor, 0, control_end), bind)
        self.assertEqual(observer.recover(generated), original)

    def test_packed_observation_uses_actual_words_before_history_updates(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        generated = observer.generate_candidate(original)
        lane = generated[generated.index(b'    void process4Character('):generated.index(b'    void process4(')]
        packed = lane[lane.index(b'            if (pairedCurve) {'):lane.index(b'                continue;')]
        for name, count in (('laneInput', 1), ('primitive', 2), ('laneQuotient', 1), ('laneMix', 1)):
            self.assertEqual(packed.count(('serial_observer::'+name+'(').encode()), count, name)
        observed = packed[packed.index(observer.BEGIN):packed.index(observer.END)]
        for value in ('d', 't', 'delta', 'shaped', 'blendDifference', 'result'):
            self.assertIn((', '+value+');').encode(), observed, 'store computed SIMD '+value)
        self.assertIn(b'for (std::size_t ch = 0; ch < 2; ++ch)', observed)
        self.assertIn(b'previousPrimitive[ch], previousDifference[ch], dc[ch], envelope, int(primitiveKind[ch])', observed)
        self.assertIn(b'primitiveKind[ch] == PrimitiveKind::curve ? 2 : 1', observed)
        positions = [observed.index(('serial_observer::'+name+'(').encode())
                     for name in ('laneInput', 'primitive', 'laneQuotient', 'laneMix')]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(packed.index(observer.END), packed.index(b'_mm_storeu_pd(dc.data(), nextDc);'))
        self.assertNotIn(b'_mm_div_pd', observed, 'observe the quotient without recomputing it')
        self.assertNotIn(b'curvePrimitive(', observed, 'observe the actual primitive results')
        self.assertEqual(observer.recover(generated), original)

    def test_packed_and_pre_anchors_fail_closed(self):
        original = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
        newline = b'\r\n' if b'\r\n' in original else b'\n'
        anchors = (b'            if (pairedCurve) {',
                   b'                _mm_storeu_pd(dc.data(), nextDc); _mm_storeu_pd(previousDifference.data(), difference);',
                   b'                continue;',
                   b'        if constexpr (Capacity == 4) {'+newline+b'            s.wetUp.up4')
        for anchor in anchors:
            self.assertEqual(original.count(anchor), 1)
            for label, replacement in (('missing', b''), ('duplicate', anchor+newline+anchor),
                                       ('malformed', anchor.replace(b' ', b'\t', 1))):
                with self.subTest(anchor=anchor, edit=label), self.assertRaises(ValueError):
                    observer.generate_candidate(original.replace(anchor, replacement, 1))

    def test_both_corpora_pins_and_stimuli(self):
        inventory = observer.source_inventory()
        for pin in ('e0d1592a45083191d3e3d14f2548ec4b032c5fed0e0109d14969859917953599',
                    '30fc96fc8b4e0f1e9708786e9cf6ae80af42212adb42f7105f4620c8e51cd588'):
            self.assertTrue(any(v['sha256'] == pin for v in inventory.values()))
        self.assertGreaterEqual(len(inventory), 70)

    def test_five_partitions_required(self):
        raw = struct.pack('<4096d', *([0.0] * 4096))
        recordings = {p: raw for p in (1, 17, 64, 127, 512)}
        self.assertEqual(len(observer.qualify_partitions(recordings, recordings)), 5)
        for bad in ({1: raw}, {**recordings, 2: raw}):
            with self.assertRaises(ValueError):
                observer.qualify_partitions(bad, recordings)
        changed = dict(recordings); changed[512] = bytes([1]) + raw[1:]
        with self.assertRaises(ValueError):
            observer.qualify_partitions(changed, recordings)

    def record(self, stage, words, **fields):
        return dict(partition=1, frame=0, slot=0, stage=stage, order=0, channel=0,
                    lane=0, subsample=0, call=0, kind=2 if stage == 34 else 0,
                    selector=0, object=-1, flags=0, ramp=0,
                    words=[struct.pack('>d', x).hex() for x in words], **fields)

    def test_first_difference_is_separate_from_equal_input_operation(self):
        # Stage 34 is actual curve quotient: current/prior primitives and delta -> q.
        a = self.record(34, [1., .5, .25, 2.])
        b = copy.deepcopy(a); b['words'][-1] = struct.pack('>d', 2.0000000000000004).hex()
        result = observer.compare_traces([a], [b])
        self.assertEqual(result['earliest_difference']['index'], 0)
        self.assertEqual(result['earliest_equal_input_operation']['index'], 0)
        b['words'][0] = struct.pack('>d', 1.0000000000000002).hex()
        self.assertIsNone(observer.compare_traces([a], [b])['earliest_equal_input_operation'])
        b = copy.deepcopy(a); b['selector'] = 1
        b['words'][-1] = struct.pack('>d', 2.0000000000000004).hex()
        self.assertIsNone(observer.compare_traces([a], [b])['earliest_equal_input_operation'])

    def test_actual_bias_normalizer_midpoint_and_unrelated_resource(self):
        for stage,inputs in ((31,[1.,.5,.75]),(33,[1.,.5,.3,.91])):
            op = self.record(stage,inputs+[2.])
            op['kind'] = 0 if stage == 31 else 1
            rows = []
            if stage == 31:
                constants = self.record(2,[1.]*6)
                constants.update(frame=-1,channel=-1,lane=-1,subsample=-1)
                rows.append(constants)
            rows.append(op); self.reindex(rows)
            peer = copy.deepcopy(rows); peer[-1]['words'][-1] = struct.pack('>d',3.).hex()
            self.assertIsNotNone(observer.compare_traces(rows,peer)['earliest_equal_input_operation'])
            for index in range(len(inputs)):
                changed = copy.deepcopy(peer)
                changed[-1]['words'][index] = struct.pack('>d',4.).hex()
                with self.subTest(stage=stage,input=index):
                    self.assertIsNone(observer.compare_traces(rows,changed)['earliest_equal_input_operation'])
        resource = self.record(7,[1.]); resource.update(frame=-1,channel=-1,lane=-1,object=18)
        gain = self.record(50,[1.,2.]); gain.update(channel=-1,lane=-1,subsample=-1)
        rows = self.reindex([resource,gain]); peer = copy.deepcopy(rows)
        peer[0]['words'][0] = struct.pack('>d',3.).hex()
        peer[1]['words'][-1] = struct.pack('>d',3.).hex()
        self.assertEqual(observer.compare_traces(rows,peer)['earliest_equal_input_operation']['index'],1)

    def test_trace_schema_and_pair_order_fail_closed(self):
        a = self.record(34, [1., .5, .25, 2.])
        c = copy.deepcopy(a); c['order'] = 1; c['call'] = 1
        for bad in ([c, a], [a, a], [a], [a, c, c]):
            with self.assertRaises(ValueError):
                observer.compare_traces([a, c], bad)
        for edit in (lambda r: r.update(frame=0.0), lambda r: r.update(channel=True),
                     lambda r: r.update(words=['7ff0000000000000']*4),
                     lambda r: r.update(words=['0x0']*4), lambda r: r.update(unknown=1)):
            bad = copy.deepcopy(a); edit(bad)
            with self.assertRaises(ValueError):
                observer.compare_traces([a], [bad])

    def test_hidden_context_must_match_before_equal_input_promotion(self):
        # Equal scalar arguments do not prove equality of consumed constants/tables.
        for stage, kind, resource in ((31,0,None),(32,1,None),(32,2,18),(51,0,19)):
            with self.subTest(stage=stage,kind=kind):
                rows = []
                constants = self.record(2, [1.,2.,3.,4.,5.,6.])
                constants.update(frame=-1,channel=-1,lane=-1,subsample=-1)
                rows.append(constants)
                if resource is not None:
                    header = self.record(15, [])
                    extent = 4096 if resource == 18 else 768
                    header.update(frame=-1,channel=-1,lane=-1,object=resource,subsample=extent)
                    rows.append(header)
                    for n in range(extent):
                        value = self.record(7, [1.])
                        value.update(frame=-1,channel=-1,lane=-1,object=resource,subsample=n)
                        rows.append(value)
                op = self.record(stage, [1.]*(4 if stage == 31 else 2))
                op.update(kind=kind)
                if stage == 32:
                    op['object'] = 0
                if stage == 51:
                    op.update(channel=-1,lane=-1,subsample=-1,flags=4)
                rows.append(op)
                self.reindex(rows)
                peer = copy.deepcopy(rows)
                peer[-1]['words'][-1] = struct.pack('>d',2.).hex()
                # These are known complete equal contexts before the hidden-only edit.
                self.assertIsNotNone(observer.compare_traces(rows,peer)['earliest_equal_input_operation'])
                peer[0 if resource is None else 2]['words'][0] = struct.pack('>d',3.).hex()
                compared = observer.compare_traces(rows,peer)
                self.assertIsNone(compared['earliest_equal_input_operation'])
                self.assertFalse(compared['earliest_equal_explicit_scalar_operation']['complete_inputs_equal'])
                # Omitted context is equally insufficient.
                short = [dict(op)]; self.reindex(short)
                changed = copy.deepcopy(short); changed[0]['words'][-1] = struct.pack('>d',2.).hex()
                self.assertIsNone(observer.compare_traces(short,changed)['earliest_equal_input_operation'])

    def test_actual_midpoint_quotient_and_factor_operands_are_exposed(self):
        generated = observer.generate_candidate((ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes())
        self.assertEqual(observer.WIDTHS[31], 4, 'include the actual computed midpoint argument')
        self.assertEqual(observer.WIDTHS[33], 5, 'include actual bias and normalizer call operands')
        self.assertIn(b'serial_observer::shapeInput(x)', generated)
        self.assertIn(b'serial_observer::exactInput(x, p, bias, normalizer)', generated)
        self.assertIn(b'serial_observer::gainResult(51, factor, db, ', generated)
        self.assertIn(b'pi, gain_math::GainCurve::decibelsToNaturalLog)', generated)

    def test_gain_context_requires_prepared_conversion_constant(self):
        rows = []
        constants = self.record(2,[1.]*observer.WIDTHS[2])
        constants.update(frame=-1,channel=-1,lane=-1,subsample=-1)
        rows.append(constants)
        header = self.record(15,[])
        header.update(frame=-1,channel=-1,lane=-1,object=19,subsample=768)
        rows.append(header)
        for n in range(768):
            value = self.record(7,[1.])
            value.update(frame=-1,channel=-1,lane=-1,object=19,subsample=n)
            rows.append(value)
        op = self.record(51,[1.,1.])
        op.update(channel=-1,lane=-1,subsample=-1,flags=4)
        rows.append(op); self.reindex(rows)
        peer = copy.deepcopy(rows); peer[-1]['words'][-1] = struct.pack('>d',2.).hex()
        self.assertIsNotNone(observer.compare_traces(rows,peer)['earliest_equal_input_operation'])
        peer[0]['words'][-1] = struct.pack('>d',2.).hex()
        self.assertIsNone(observer.compare_traces(rows,peer)['earliest_equal_input_operation'])
        self.assertEqual(observer.WIDTHS[2],6, 'record actual prepared gain decibelsToNaturalLog')

    def reindex(self, rows):
        calls = {}
        for order,row in enumerate(rows):
            key = tuple(row[k] for k in ('frame','slot','stage','channel','lane','subsample','object'))
            row['order'] = order
            row['call'] = calls.get(key,0)
            calls[key] = row['call']+1
        return rows

    def test_incomplete_prefix_cannot_be_read_as_capture(self):
        path = self.work/'trace.jsonl'
        for records in ([], [self.record(34, [1., .5, .25, 2.])]):
            path.write_text(''.join(json.dumps(r)+'\n' for r in records))
            with self.assertRaises(ValueError):
                observer.read_trace(path)

    def full_prefix(self, wide=True):
        # Independent grammar fixture: zero-valued boundaries, original routes and modes.
        rows = []
        calls = {}
        def add(stage, frame=-1, slot=-1, lane=-1, subsample=-1, channel=-1,
                kind=0, selector=0, object=-1, flags=0, width=None):
            key = (frame,slot,stage,channel,lane,subsample,object)
            call = calls.get(key, 0); calls[key] = call+1
            rows.append(dict(partition=1,frame=frame,slot=slot,stage=stage,order=len(rows),
                             channel=channel,lane=lane,subsample=subsample,call=call,kind=kind,
                             selector=selector,object=object,flags=flags,ramp=0,
                             words=['0000000000000000']*(observer.WIDTHS[stage] if width is None else width)))
        def state(frame, slot, mode, wide):
            add(3,frame,slot,kind=mode,flags=int(wide))
            add(16,frame,slot)
            for n in range(7):
                add(5,frame,slot,object=n)
            for n in range(4):
                add(4,frame,slot,lane=n)
                if slot == 0 and n == 0:
                    rows[-1]['words'][1] = struct.pack('>d',1.).hex()
                add(6,frame,slot,lane=n)
                add(5,frame,slot,lane=n,object=7+2*n)
                add(5,frame,slot,lane=n,object=8+2*n)
        for slot in range(8):
            for stage in (51,51,50,51,51,51,51):
                add(stage,slot=slot,flags=4 if stage == 51 else 0)
            state(-1,slot,0,False)
            add(2,slot=slot)
            state(-1,slot,0,wide)
            if slot == 0:
                # Independent source manifest: JUCE designed 5/4,3/3,2/2 sections,
                # six fixed FIRs, 512*8 primitive coefficients, 768 gain centers.
                for object,extent in enumerate((5,4,5,4,3,3,3,3,2,2,2,2,257,129,65,33,17,17,4096,768)):
                    add(15,slot=0,subsample=extent,object=object)
                    for n in range(extent):
                        add(7,slot=0,subsample=n,object=object)
            for _ in range(4):
                add(50,slot=slot)
                add(51,slot=slot,flags=4)
            state(-1,slot,slot%2,wide)
            state(-1,slot,slot%2,wide)
        for frame in range(511):
            add(0,frame)
            if frame in (63,64,65,127):
                add(1,frame,2,selector=0 if frame in (63,127) else 1,kind=int(frame == 63))
                if frame in (64,65):
                    state(frame,2,int(frame == 65),False)
            for slot in range(8):
                if slot == 2 and 63 <= frame < 127:
                    continue
                mode = int(slot%2 == 1 or slot == 2 and frame >= 127)
                add(8,frame,slot)
                if slot == 2 and frame >= 127:
                    for _ in range(4):
                        add(50,frame,slot)
                        add(51,frame,slot,flags=4)
                for _ in range(2 if slot == 2 and frame >= 127 else 1):
                    state(frame,slot,mode,wide)
                add(10,frame,slot)
                for n in range(4):
                    add(11,frame,slot,subsample=n)
                if mode and wide:
                    for stage in (12,13):
                        for obj in (0,2,4,6,1,3,5,7,8):
                            add(stage,frame,slot,object=obj)
                    for n in range(4):
                        for band in range(3):
                            add(14,frame,slot,subsample=n,object=band)
                elif mode:
                    for n in range(4):
                        for obj in (10,12,11,13,14,16,15,17,18,19,21,20,22,23,25,24,26,27):
                            for stage in (20,21):
                                for ch in range(2):
                                    add(stage,frame,slot,object=obj,channel=ch)
                for lane in (range(1,4) if mode else (0,)):
                    for n in range(4):
                        if slot == 0:
                            for obj in (2,3):
                                for stage in (20,21):
                                    for ch in range(2):
                                        add(stage,frame,slot,lane,n,ch,object=obj)
                        for ch in range(2):
                            quotient = frame == slot == 0 and n in (0,1)
                            kind = ch+1 if quotient else 0
                            prior_kind = kind if quotient and n == 1 else 0
                            add(30,frame,slot,lane,n,ch,kind=prior_kind)
                            if quotient:
                                add(32,frame,slot,lane,n,ch,kind=kind,object=0)
                                add(32,frame,slot,lane,n,ch,kind=kind,object=1,flags=2 if n == 1 else 1)
                                add(33+ch,frame,slot,lane,n,ch,kind=kind,flags=prior_kind)
                            else:
                                add(31,frame,slot,lane,n,ch)
                            add(35,frame,slot,lane,n,ch)
                for n in range(4):
                    for ch in range(2):
                        add(40,frame,slot,subsample=n,channel=ch)
                add(41,frame,slot)
                for ch in range(2):
                    add(42,frame,slot,channel=ch)
                add(9,frame,slot)
        return rows

    def test_original_resume_event_required_and_ordered(self):
        rows = self.full_prefix()
        with (ROOT/'probes/saturation/contracts/fixtures/candidate-20261021/serial-events.csv').open(newline='') as source:
            events = [(int(r['frame']),r['operation'],r['slot'],r['bypassed'])
                      for r in csv.DictReader(source) if int(r['frame']) <= 510]
        self.assertEqual(events, [(63,'bypass','2','1'),(64,'settings','2',''),
                                  (65,'settings','2',''),(127,'bypass','2','0')])
        observer.validate_records(rows)
        observer.validate_coverage(rows)
        at = next(i for i,r in enumerate(rows) if r['stage'] == 1 and r['frame'] == 127)
        self.assertEqual((rows[at]['slot'],rows[at]['selector'],rows[at]['kind']), (2,0,0))
        for edit in ('missing','duplicate','wrong_kind','wrong_selector','late'):
            bad = [dict(r) for r in rows]
            if edit == 'missing': bad.pop(at)
            elif edit == 'duplicate': bad.insert(at,dict(bad[at]))
            elif edit == 'wrong_kind': bad[at]['kind'] = 1
            elif edit == 'wrong_selector': bad[at]['selector'] = 1
            else:
                moved = bad.pop(at)
                destination = next(i for i,r in enumerate(bad) if r['frame'] == 127 and r['slot'] == 3)
                bad.insert(destination,moved)
            self.reindex(bad)
            observer.validate_records(bad)
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                observer.validate_coverage(bad)

    def test_historical_producer_phase_is_explicit_and_byte_bound(self):
        directory = self.work/'producer'
        descriptor = observer.generate_directory(directory)
        current = observer.source_inventory()
        snapshot = self.work/'producer-tool.py'
        raw = (ROOT/'tools/saturation_serial_observer.py').read_bytes()+b'\n# prior analyzer phase\n'
        snapshot.write_bytes(raw)
        descriptor['sources']['tools/saturation_serial_observer.py'] = observer.revision.identity(raw)
        read = lambda p,limit: observer.revision.read_regular(Path(p),limit)
        with self.assertRaises(ValueError):
            observer.validate_producer_source_phase(descriptor,current,directory,None,read)
        phase = observer.validate_producer_source_phase(descriptor,current,directory,snapshot,read)
        self.assertTrue(phase['generator_and_hooks_unchanged'])
        self.assertNotEqual(phase['producer_tool'],phase['analyzer_tool'])
        for edit in ('snapshot','generator','header','generated'):
            changed = copy.deepcopy(descriptor)
            snapshot.write_bytes(raw)
            if edit == 'snapshot': snapshot.write_bytes(raw+b'# altered\n')
            elif edit == 'generator':
                changed_raw = raw.replace(b'def generate_candidate(original: bytes) -> bytes:',
                                         b'def generate_candidate(original: bytes) -> bytes:\n    raise ValueError("changed")')
                self.assertNotEqual(changed_raw,raw)
                snapshot.write_bytes(changed_raw)
                changed['sources']['tools/saturation_serial_observer.py'] = observer.revision.identity(changed_raw)
            elif edit == 'header':
                changed['sources']['probes/saturation/contracts/SaturationSerialObserver.h']['sha256'] = '0'*64
            else:
                (directory/'SaturationCandidateObserved.cpp').write_bytes(b'altered')
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                observer.validate_producer_source_phase(changed,current,directory,snapshot,read)

    def test_prior_cache_kind_and_flags_must_agree(self):
        rows = self.full_prefix()
        observer.validate_records(rows)
        observer.validate_coverage(rows)
        prior_indices = [i for i,r in enumerate(rows) if r['stage'] == 32 and r['object'] == 1]
        self.assertEqual({rows[i]['flags'] for i in prior_indices}, {1,2})
        for index in prior_indices:
            # Both exact/curve paths must tag cache reuse from the recorded prior kind.
            bad = list(rows)
            bad[index] = dict(rows[index],flags=3-rows[index]['flags'])
            observer.validate_records(bad)
            with self.subTest(kind=rows[index]['kind'],prior_flags=bad[index]['flags']), self.assertRaises(ValueError):
                observer.validate_coverage(bad)
            bad = list(rows)
            bad[index+1] = dict(rows[index+1],flags=(rows[index+1]['flags']+1)%3)
            observer.validate_records(bad)
            with self.subTest(kind=rows[index]['kind'],quotient_flags=bad[index+1]['flags']), self.assertRaises(ValueError):
                observer.validate_coverage(bad)
            bad = list(rows)
            bad[index-2] = dict(rows[index-2],kind=3)
            bad[index+1] = dict(rows[index+1],flags=3)
            bad[index] = dict(rows[index],flags=1)
            observer.validate_records(bad)
            with self.subTest(kind=rows[index]['kind'],prior_kind=3), self.assertRaises(ValueError):
                observer.validate_coverage(bad)

    def test_whole_wide_block_and_contiguous_groups_cannot_move(self):
        rows = self.full_prefix()
        for predicate in (
                lambda r: r['frame'] == 0 and r['slot'] == 1 and r['stage'] == 13,
                lambda r: r['frame'] == -1 and r['slot'] == 0 and r['stage'] in (7,15)):
            moved = [dict(r) for r in rows if predicate(r)]
            bad = [dict(r) for r in rows if not predicate(r)]
            at = next(i for i,r in enumerate(bad) if r['frame'] == 0 and r['slot'] == 1 and r['stage'] == 9)+1
            bad[at:at] = moved
            self.reindex(bad)
            observer.validate_records(bad)
            with self.subTest(moved_stage=moved[0]['stage']), self.assertRaises(ValueError):
                observer.validate_coverage(bad)

    def test_cross_stage_order_and_scalar_fallback(self):
        for wide in (True,False):
            rows = self.full_prefix(wide)
            observer.validate_records(rows)
            observer.validate_coverage(rows)
            # Keep each filtered stage list intact; violate its execution relationship.
            for stage,destination in ((3,10),(50,2),(31,40),(40,31)):
                frame,slot = (-1,0) if stage == 50 else (0,0)
                indices = [i for i,r in enumerate(rows) if r['frame'] == frame and r['slot'] == slot and r['stage'] == stage]
                index = indices[0]
                bad = list(rows)
                moved = bad.pop(index)
                at = next(i for i,r in enumerate(bad) if r['frame'] == frame and r['slot'] == slot and r['stage'] == destination)
                bad.insert(at,moved)
                with self.subTest(wide=wide,stage=stage), self.assertRaises(ValueError):
                    observer.validate_coverage(bad)

    def test_resource_header_and_tail_cannot_be_truncated_together(self):
        rows = self.full_prefix()
        for obj,extent in ((0,5),(12,257),(18,4096),(19,768)):
            bad = [dict(r) for r in rows if not (r['stage'] == 7 and r['object'] == obj and r['subsample'] == extent-1)]
            header = next(r for r in bad if r['stage'] == 15 and r['object'] == obj)
            header['subsample'] -= 1
            self.reindex(bad)
            observer.validate_records(bad)
            with self.subTest(object=obj), self.assertRaises(ValueError):
                observer.validate_coverage(bad)

    def test_complete_prefix_and_missing_duplicated_or_reordered_stage_refused(self):
        rows = self.full_prefix()
        observer.validate_records(rows)
        observer.validate_coverage(rows)
        # Reindex corruption so the independent coverage check, not just order, catches it.
        for stage in (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,20,21,30,31,32,33,34,35,40,41,42,50,51):
            index = next(i for i,r in enumerate(rows) if r['stage'] == stage)
            bad = rows[:index]+rows[index+1:]
            with self.subTest(stage=stage), self.assertRaises(ValueError):
                observer.validate_coverage(bad)
        for stage in (1,2,4,8,11,30,31,40):
            index = next(i for i,r in enumerate(rows) if r['stage'] == stage)
            bad = rows[:index]+[rows[index]]+rows[index:]
            with self.subTest(duplicate_stage=stage), self.assertRaises(ValueError):
                observer.validate_coverage(bad)
        first = next(i for i,r in enumerate(rows) if r['stage'] == 11)
        bad = list(rows); bad[first],bad[first+1] = bad[first+1],bad[first]
        with self.assertRaises(ValueError):
            observer.validate_coverage(bad)
        bad = [r for r in rows if not (r['frame'] == 0 and r['slot'] == 0 and
                                       r['stage'] in (20,21) and r['object'] == 2)]
        with self.assertRaises(ValueError):
            observer.validate_coverage(bad)
        extra = dict(rows[0],stage=21,words=['0000000000000000']*3)
        with self.assertRaises(ValueError):
            observer.validate_coverage([extra]+rows)
        path = self.work/'complete-trace.jsonl'
        with path.open('w',encoding='utf-8') as stream:
            for row in rows:
                stream.write(json.dumps(row,separators=(',',':'))+'\n')
        self.assertEqual(len(observer.read_trace(path)), len(rows))

    def test_generation_freshness_before_mutation_and_source_descriptor(self):
        target = self.work/'generated'
        observer.generate_directory(target)
        descriptor = json.loads((target/'source.json').read_text())
        self.assertTrue(descriptor['candidate_recovered_exact'])
        self.assertTrue(descriptor['fixture_recovered_exact'])
        self.assertEqual((target/'Candidate.recovered.cpp').read_bytes(),
                         (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes())
        self.assertTrue((target/'SaturationSerialSource.h').is_file(), 'compiled descriptor binding is required')
        self.assertEqual((target/'SaturationSerialSource.h').read_text(),
                         '#define RACKSYNTH_SERIAL_SOURCE_SHA256 "'+
                         hashlib.sha256((target/'source.json').read_bytes()).hexdigest()+'"\n')
        before = {p.name: p.read_bytes() for p in target.iterdir()}
        with self.assertRaises(ValueError):
            observer.generate_directory(target)
        self.assertEqual(before, {p.name: p.read_bytes() for p in target.iterdir()})
        with self.assertRaises(ValueError):
            observer.generate_directory(Path('relative-output'))

    def test_missing_private_evidence_is_explicit_and_output_stays_fresh(self):
        output = self.work/'result.json'
        with self.assertRaisesRegex(ValueError, 'capture|evidence|cannot read'):
            observer.analyze(self.work/'Release', self.work/'Debug', output)
        self.assertFalse(output.exists())
        output.write_text('preserve')
        with self.assertRaises(ValueError):
            observer.analyze(self.work/'Release', self.work/'Debug', output)
        self.assertEqual(output.read_text(), 'preserve')

    def test_cmake_target_is_distinct_normal_and_generated(self):
        text = (ROOT/'CMakeLists.txt').read_text()
        start = text.index('set(saturation_serial_observer ')
        end = text.index('# End normal serial observer.', start)
        block = text[start:end]
        self.assertIn('RackSynthSaturationSerialObserver', block)
        self.assertIn('SaturationCandidateObserved.cpp', block)
        self.assertIn('SaturationFixtureObserved.cpp', block)
        self.assertIn('EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE', block)
        self.assertNotIn('AIRACK_SATURATION_TRANSITIONS', block)
        self.assertNotIn('AIRACK_SATURATION_NUMERICAL_TEST', block)
        self.assertNotIn('RackSynthSaturationCandidate)', block)


if __name__ == '__main__':
    unittest.main()
