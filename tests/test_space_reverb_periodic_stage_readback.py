import csv,io,unittest
from tools import space_reverb_periodic_stage_readback as reader
class PeriodicStageReadbackTests(unittest.TestCase):
    def fixture(self):
        columns=('ordinal','release','start','begin','finish','cycles','maximum_rooms','room_copy','post_admission','cold_install','valid','allocations','frees')+tuple(n+'_ns' for n in reader.NAMES)+tuple(n+'_calls' for n in reader.NAMES)
        buffer=io.StringIO();buffer.write('MODAL_PERIODIC_STAGE_V1,row=10,rate=96000,host=64,factor=2,modes=128,slots=8,warm=15000,expected=4096,frequency=1000000000,paced=1,opened=1,closed=1,priority_before=0,priority_during=15,priority_after=0,valid=1,interrupted=0,reason=none,capacity_acceptance=0\n');writer=csv.DictWriter(buffer,fieldnames=columns,lineterminator='\n');writer.writeheader()
        for n in range(4096):
            release=10**9+(n+1)*10**9*64//96000;row={k:0 for k in columns};row.update(ordinal=n,release=release,start=release,begin=release+3000,finish=release+200000,cycles=600000,maximum_rooms=8,valid=1)
            for name in reader.NAMES:row[name+'_ns']=1000;row[name+'_calls']=1 if name=='publish' else 0 if name=='room_fade' else 8
            writer.writerow(row)
        return buffer.getvalue()
    def test_complete_direction_only(self):
        report=reader.decode(self.fixture());self.assertEqual(report['all']['samples'],4096);self.assertFalse(report['capacity_acceptance']);self.assertEqual(report['all']['total']['mean_us'],200)
    def test_rejects_bad_contracts(self):
        original=self.fixture()
        for before,after in [('valid=1,interrupted=0','valid=0,interrupted=0'),('capacity_acceptance=0','capacity_acceptance=1'),('priority_after=0','priority_after=15'),('expected=4096','expected=4095'),('capture_ns','unknown_ns')]:
            with self.subTest(before=before),self.assertRaises(ValueError):reader.decode(original.replace(before,after,1))
        lines=original.splitlines();
        with self.assertRaises(ValueError):reader.decode('\n'.join(lines[:-1]))
        columns=lines[1].split(',');row=lines[2].split(',')
        for field,value in [('capture_calls','7'),('room_fade_calls','1'),('allocations','1'),('begin','0'),('maximum_rooms','9'),('cold_install','1')]:
            mutated=list(row);mutated[columns.index(field)]=value
            with self.subTest(field=field),self.assertRaises(ValueError):reader.decode('\n'.join(lines[:2]+[','.join(mutated)]+lines[3:]))
if __name__=='__main__':unittest.main()
