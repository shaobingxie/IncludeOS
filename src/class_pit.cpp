#define DEBUG 
#include <os>
#include <class_pit.hpp>
#include <class_irq_handler.hpp>
#include <hw/cpu_freq_sampling.hpp>

// Bit 0-3: Mode 0 - "Interrupt on terminal count"
// Bit 4-5: Both set, access mode "Lobyte / Hibyte"
const uint8_t PIT_one_shot = 0x30;  
const uint8_t PIT_mode_register = 0x43;
const uint8_t  PIT_chan0 = 0x40;

// Private, singular instance
PIT* PIT::pit_ = 0;

PIT::Mode PIT::current_mode_ = NONE;
PIT::Mode PIT::temp_mode_ = NONE;
uint64_t PIT::IRQ_counter_ = 0;
uint64_t PIT::prev_timestamp_ = 0;


extern "C" double _CPUFreq_;
extern "C" uint16_t _cpu_sampling_freq_divider_;

uint16_t PIT::current_freq_divider_ = 0;
uint16_t PIT::temp_freq_divider_ = 0;

uint64_t PIT::millisec_counter = 0;

extern "C" void irq_timer_entry();

using namespace std::chrono;


void PIT::disable_regular_interrupts()
{   
  oneshot(1);
}

PIT::~PIT(){
  delete pit_;
}

PIT::PIT(){
  debug("<PIT> Instantiating. \n");
  
  auto handler(IRQ_handler::irq_delegate::from<PIT,&PIT::irq_handler>(this));
  
  IRQ_handler::subscribe(0, handler);
}


void PIT::estimateCPUFrequency(){

  debug("<PIT EstimateCPUFreq> Saving state: curr_freq_div %i \n",current_freq_divider_);
  // Save PIT-state
  temp_mode_ = current_mode_;
  temp_freq_divider_ = current_freq_divider_;
  
  auto prev_irq_handler = IRQ_handler::get_handler(32);

  debug("<PIT EstimateCPUFreq> Sampling\n");
  IRQ_handler::set_handler(32, cpu_sampling_irq_entry);

  // GO!
  set_mode(RATE_GEN);  
  set_freq_divider(_cpu_sampling_freq_divider_);    

  // BLOCKING call to external measurment. 
  calculate_cpu_frequency();
    
  debug("<PIT EstimateCPUFreq> Done. Result: %f \n", _CPUFreq_);
  
  set_mode(temp_mode_);
  set_freq_divider(temp_freq_divider_);

  IRQ_handler::set_handler(32, prev_irq_handler);
}

MHz PIT::CPUFrequency(){
  if (! _CPUFreq_)
    estimateCPUFrequency();
  
  return MHz(_CPUFreq_);
}





void PIT::onTimeout(std::chrono::milliseconds msec, timeout_handler handler){
  
  if (msec < 1ms) panic("Can't wait less than 1 ms. ");
  
  debug("<PIT sec> setting a %i ms. timer \n", msec);
    
  /**
    @Todo
    * Queue the timer
    * Make sure there's an appropriate interrupt coming */  
  
  
  if (current_mode_ != RATE_GEN)
    set_mode(RATE_GEN);
  
  if (current_freq_divider_ != millisec_interval)
    set_freq_divider(millisec_interval);
  
  
  auto freq = CPUFrequency();
  debug("CPUFrequency: MHz: %f KHz: %f Hz: %f \n", freq, KHz(freq), Hz(freq));
  
  auto cycles_pr_millisec = KHz(freq);
  debug("KHz: %f Cycles to wait: %f \n",cycles_pr_millisec, cycles_pr_millisec.count() * msec);
  
  auto now = OS::cycles_since_boot();
  Timer t {handler, now, now + uint64_t(cycles_pr_millisec.count() * msec.count())};
  
  timers_[millisec_counter + msec.count()] = t;   
        
};


uint8_t PIT::read_back(uint8_t channel){
  const uint8_t READ_BACK_CMD = 0xc2;
  
  OS::outb(PIT_mode_register, READ_BACK_CMD );
  
  auto res = OS::inb(PIT_chan0);
  
  debug("STATUS: %#x \n", res);
  
  return res;
  
}

void PIT::irq_handler(){

  IRQ_counter_ ++;
  
  if (current_freq_divider_ == millisec_interval)
    millisec_counter++;
  
  if (millisec_counter % 100 == 0)
    OS::rsprint(".");
 
  
  for (auto it = timers_.begin(); it != timers_.end(); it++)
    if (it->first <= millisec_counter) {
      // Timer expired - run its handler
      it->second.handler();      
      timers_.erase(it);
      
      if (timers_.size() == 0){	
	// Reset the timer	
	oneshot(1);	
	// Escape iterator death
	break;
      }
    } 
  
  IRQ_handler::eoi(0);

}

void PIT::init(){
  debug("<PIT> Initializing @ frequency: %16.16f MHz. Assigning myself to all timer interrupts.\n ", frequency());  
  PIT::disable_regular_interrupts();
  //IRQ_handler::enable_irq(32);
}

void PIT::set_mode(Mode mode){
  // Channel is the last two bits in the PIT mode register
  // ...we always use channel 0
  auto channel = 0x00;  
  uint8_t config = mode | LO_HI | channel;
  debug("<PIT::set_mode> Setting mode %#x, config: %#x \n", mode, config);
  
  OS::outb(PIT_mode_register, config);
  current_mode_ = mode;

}

void PIT::set_freq_divider(uint16_t freq_divider){    
  union{
    uint16_t whole;
    uint8_t part[2];
  }data{freq_divider};  

  // Send frequency hi/lo to PIT
  OS::outb(PIT_chan0, data.part[0]);
  OS::outb(PIT_chan0, data.part[1]);

  current_freq_divider_ = freq_divider;

}

void PIT::oneshot(uint16_t t){
    
  // Enable 1-shot mode
  set_mode(ONE_SHOT);  
  
  // Set a frequency for shot
  set_freq_divider(t);
}

