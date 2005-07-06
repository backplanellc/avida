//////////////////////////////////////////////////////////////////////////////
// Copyright (C) 1993 - 2003 California Institute of Technology             //
//                                                                          //
// Read the COPYING and README files, or contact 'avida@alife.org',         //
// before continuing.  SOME RESTRICTIONS MAY APPLY TO USE OF THIS FILE.     //
//////////////////////////////////////////////////////////////////////////////

#include "organism.hh"

#include "config.hh"
#include "cpu_defs.hh"
#include "environment.hh"
#include "functions.hh"
#include "genome.hh"
#include "genome_util.hh"
#include "hardware_base.hh"
#include "inject_genotype.hh"
#include "inst_set.hh"
#include "inst_util.hh"
#include "org_message.hh"
#include "string_util.hh"
#include "tArray.hh"
#include "tools.hh"
#include "tList.hh"

#include <iomanip>

using namespace std;

///////////////
//  cOrganism
///////////////

int cOrganism::instance_count(0);


cOrganism::cOrganism(const cGenome & in_genome,
		     const cPopulationInterface & in_interface,
		     const cEnvironment & in_environment)
  : genotype(NULL)
  , phenotype(in_environment)
  , initial_genome(in_genome)
  , mut_info(in_environment.GetMutationLib(), in_genome.GetSize())
  , pop_interface(in_interface)
  , input_pointer(0)
  , input_buf(INPUT_BUF_SIZE)
  , output_buf(OUTPUT_BUF_SIZE)
  , send_buf(SEND_BUF_SIZE)
  , receive_buf(RECEIVE_BUF_SIZE)
  , sent_value(0)
  , sent_active(false)
  , test_receive_pos(0)
  , max_executed(-1)
  , lineage_label(-1)
  , lineage(NULL)
  , inbox(0)
  , sent(0)
  , is_running(false)
{
  // Initialization of structures...
  hardware = pop_interface.NewHardware(this);
  cpu_stats.Setup(hardware->GetNumInst());
  instance_count++;
  pop_interface.SetCellID(-1);  // No cell at the moment...

  if (cConfig::GetDeathMethod() > 0) {
    max_executed = cConfig::GetAgeLimit();
    if (cConfig::GetAgeDeviation() > 0.0) {
      max_executed +=
	(int) (g_random.GetRandNormal() * cConfig::GetAgeDeviation());
    }
    if (cConfig::GetDeathMethod() == DEATH_METHOD_MULTIPLE) {
      max_executed *= initial_genome.GetSize();
    }

    // max_executed must be positive or an organism will not die!
    if (max_executed < 1) max_executed = 1;
  }
}


cOrganism::~cOrganism()
{
  assert(is_running == false);
  pop_interface.RecycleHardware(hardware);
  instance_count--;
}


double cOrganism::GetTestFitness()
{
  return pop_interface.TestFitness();
}
  
int cOrganism::ReceiveValue()
{
  const int out_value = pop_interface.ReceiveValue();
  receive_buf.Add(out_value);
  return out_value;
}


void cOrganism::DoInput(const int value)
{
  input_buf.Add(value);
  phenotype.TestInput(input_buf, output_buf);
}

void cOrganism::DoOutput(const int value)
{
  const tArray<double> & resource_count = pop_interface.GetResources();

  tList<tBuffer<int> > other_input_list;
  tList<tBuffer<int> > other_output_list;

  // If tasks require us to consider neighbor inputs, collect them...
  if (phenotype.GetEnvironment().GetTaskLib().UseNeighborInput() == true) {
    const int num_neighbors = pop_interface.GetNumNeighbors();
    for (int i = 0; i < num_neighbors; i++) {
      pop_interface.Rotate();
      cOrganism * cur_neighbor = pop_interface.GetNeighbor();
      if (cur_neighbor == NULL) continue;

      other_input_list.Push( &(cur_neighbor->input_buf) );
    }
  }

  // If tasks require us to consider neighbor outputs, collect them...
  if (phenotype.GetEnvironment().GetTaskLib().UseNeighborOutput() == true) {
    const int num_neighbors = pop_interface.GetNumNeighbors();
    for (int i = 0; i < num_neighbors; i++) {
      pop_interface.Rotate();
      cOrganism * cur_neighbor = pop_interface.GetNeighbor();
      if (cur_neighbor == NULL) continue;

      other_output_list.Push( &(cur_neighbor->output_buf) );
    }
  }

  // Do the testing of tasks performed...
  output_buf.Add(value);
  tArray<double> res_change(resource_count.GetSize());
  tArray<int> insts_triggered;
  phenotype.TestOutput(input_buf, output_buf, send_buf, receive_buf,
		       resource_count, res_change, insts_triggered,
		       other_input_list, other_output_list);
  pop_interface.UpdateResources(res_change);

  for (int i = 0; i < insts_triggered.GetSize(); i++) {
    const int cur_inst = insts_triggered[i];
    hardware->ProcessBonusInst( cInstruction(cur_inst) );
  }
}

void cOrganism::SendMessage(cOrgMessage & mess)
{
  if(pop_interface.SendMessage(mess))
    sent.Add(mess);
  else
    {
      //perhaps some kind of message error buffer?
    }
}

bool cOrganism::ReceiveMessage(cOrgMessage & mess)
{
  inbox.Add(mess);
  return true;
}

bool cOrganism::InjectParasite(const cGenome & injected_code)
{
  return pop_interface.InjectParasite(this, injected_code);
}

bool cOrganism::InjectHost(const cCodeLabel & label, const cGenome & injected_code)
{
  return hardware->InjectHost(label, injected_code);
}

void cOrganism::AddParasite(cInjectGenotype * in_genotype)
{
  parasites.push_back(in_genotype);
}

cInjectGenotype & cOrganism::GetParasite(int x)
{
  return *parasites[x];
}

int cOrganism::GetNumParasites()
{
  return parasites.size();
}

void cOrganism::ClearParasites()
{
  parasites.clear();
}

int cOrganism::OK()
{
  if (!hardware->OK()) return false;
  if (!phenotype.OK()) return false;

  return true;
}


double cOrganism::CalcMeritRatio()
{
  const double age = (double) phenotype.GetAge();
  const double merit = phenotype.GetMerit().GetDouble();
  return (merit > 0.0) ? (age / merit ) : age;
}


bool cOrganism::GetTestOnDivide() const { return pop_interface.TestOnDivide();}
bool cOrganism::GetFailImplicit() const { return cConfig::GetFailImplicit(); }

bool cOrganism::GetRevertFatal() const { return cConfig::GetRevertFatal(); }
bool cOrganism::GetRevertNeg()   const { return cConfig::GetRevertNeg(); }
bool cOrganism::GetRevertNeut()  const { return cConfig::GetRevertNeut(); }
bool cOrganism::GetRevertPos()   const { return cConfig::GetRevertPos(); }

bool cOrganism::GetSterilizeFatal() const{return cConfig::GetSterilizeFatal();}
bool cOrganism::GetSterilizeNeg()  const { return cConfig::GetSterilizeNeg(); }
bool cOrganism::GetSterilizeNeut() const { return cConfig::GetSterilizeNeut();}
bool cOrganism::GetSterilizePos()  const { return cConfig::GetSterilizePos(); }


void cOrganism::PrintStatus(ostream & fp, const cString & next_name)
{
  fp << "---------------------------" << endl;
  hardware->PrintStatus(fp);
  phenotype.PrintStatus(fp);
  fp << "---------------------------" << endl;
  fp << "ABOUT TO EXECUTE: " << next_name << endl;
}


bool cOrganism::Divide_CheckViable()
{
  // Make sure required task (if any) has been performed...
  const int required_task = cConfig::GetRequiredTask();
  const int immunity_task = cConfig::GetImmunityTask();
//cout << "req=" << required_task << "," <<phenotype.GetCurTaskCount()[required_task]<< " "; 
//cout << "im=" << immunity_task << "," <<phenotype.GetCurTaskCount()[immunity_task]<< endl; 
  if (required_task != -1 &&
      phenotype.GetCurTaskCount()[required_task] == 0) { 
    if (immunity_task==-1 || 
	phenotype.GetCurTaskCount()[immunity_task] == 0) {
      Fault(FAULT_LOC_DIVIDE, FAULT_TYPE_ERROR,
	    cStringUtil::Stringf("Lacks required task (%d)",
			         cConfig::GetRequiredTask()));
      return false; //  (divide fails)
    } 
  }

  const int required_reaction = cConfig::GetRequiredReaction();
  if (required_reaction != -1 &&
      phenotype.GetCurTaskCount()[required_reaction] == 0) {
    Fault(FAULT_LOC_DIVIDE, FAULT_TYPE_ERROR,
	  cStringUtil::Stringf("Lacks required reaction (%d)",
			       cConfig::GetRequiredReaction()));
    return false; //  (divide fails)
  }

  // Make sure the parent is fertile
  if ( phenotype.IsFertile() == false ) {
    Fault(FAULT_LOC_DIVIDE, FAULT_TYPE_ERROR, "Infertile organism");
    return false; //  (divide fails)
  }

  return true;  // Organism has no problem with divide...
}


// This gets called after a successful divide to deal with the child. 
// Returns true if parent lives through this process.

bool cOrganism::ActivateDivide()
{
  // Activate the child!  (Keep Last: may kill this organism!)
  return pop_interface.Divide(this, child_genome);
}


void cOrganism::Fault(int fault_loc, int fault_type, cString fault_desc)
{
  (void) fault_loc;
  (void) fault_type;
  (void) fault_desc;

#ifdef FATAL_ERRORS
  if (fault_type == FAULT_TYPE_ERROR) {
    phenotype.IsFertile() = false;
  }
#endif

#ifdef FATAL_WARNINGS
  if (fault_type == FAULT_TYPE_WARNING) {
    phenotype.IsFertile() = false;
  }
#endif

#ifdef BREAKPOINTS
  phenotype.SetFault(fault_desc);
#endif

  phenotype.IncErrors();
}


//// Save and Load ////
void cOrganism::SaveState(ofstream & fp)
{
  assert(fp.good());

  fp <<"cOrganism"<<endl;

  //// Save If it is alive ////
  if( genotype == NULL ){
    fp <<false<<endl;
  }
  else{  // if there is a genotype here (ie. not dead)
    fp <<true<<endl;

    fp << input_pointer;

    // IO buffers
    input_buf.SaveState(fp);
    output_buf.SaveState(fp);

    //// Save Genotype Genome ////
//    fp << genotype->GetLength() << endl;
    
//      cInstUtil::SaveInternalGenome(fp, hardware->GetInstSet(),
//  				  genotype->GetGenome());

    //// Save Actual Creature Memory & MemFlags ////
//   fp <<hardware->GetMemory().GetSize()<<endl;
//   cInstUtil::PrintGenome(hardware->GetInstSet(), hardware->GetMemory(), fp);
//      fp <<"|"; // marker
//      for( int i=0; i<hardware->GetMemory().GetSize(); ++i ){
//        fp << hardware->GetMemory().GetFlags(i);
//      }
//      fp <<endl;

    //// Save Hardware (Inst_Pointer, Stacks, and the like)
    hardware->SaveState(fp);

    //// Save Phenotype  ////
    phenotype.SaveState(fp);

  }
}


void cOrganism::LoadState(ifstream & fp)
{
  hardware->Reset();

  assert(fp.good());

  cString foo;
  fp >>foo;
  assert( foo == "cOrganism" );

  //// Is there a creature there ////
  bool alive_flag = 0;
  fp >>alive_flag;
  if( alive_flag ){

    // IO buffers
    //    fp.get(input_pointer);
    input_buf.LoadState(fp);
    output_buf.LoadState(fp);

    //// Load Genotype ////
    cGenome in_code =
      cInstUtil::LoadInternalGenome(fp, hardware->GetInstSet());
//    cGenotype * new_genotype = environment->AddGenotype(in_code);
//    ChangeGenotype(new_genotype);

    //// Load Actual Creature Memory & MemFlags ////
//    {	
//      in_code = cInstUtil::LoadInternalGenome(fp, hardware->GetInstSet());
//      hardware->GetMemory() = in_code;
//      CA_FLAG_TYPE flags;
//      char marker;  fp >>marker;  assert( marker == '|' );
//      for( int i=0; i<hardware->GetMemory().GetSize(); ++i ){
//    	  fp.get(flags);
//  	  hardware->SetMemFlags(i, flags);
//      }
//    }

    //// Load Hardware (Inst_Pointer, Stacks, and the like)
    hardware->LoadState(fp);

    //// Load Phenotype  ////
    assert(fp.good());
//      phenotype.Clear(genotype->GetLength());
    phenotype.LoadState(fp);

    //// Adjust Time Slice ////
//    environment->AdjustTimeSlice();

  } // end if not a dead creature
}
