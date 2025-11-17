#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// Process Control Block
typedef struct Process {
  int pid;              
  int arrival_time;     
  int cpu_exec_time;    
  int remaining_time;   
  int interval_time;    
  int io_time;          
  int current_priority; 
  int first_priority;   

  // Aging için
  int time_in_ready_queue; 
  int last_ready_time;  

  // Execution tracking için
  int current_burst_time; 
  int io_completion_time; 

  struct Process *next; 
} Process;

typedef struct Queue {
  Process *head;
  Process *tail;
  int size;
} Queue;

volatile int current_clock = 0;
volatile int simulation_complete = 0;

Queue ready_queue;  
Queue waiting_queue; 

Process *all_processes = NULL;
int num_processes = 0;
int next_process_index = 0;

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_queue(Queue *q) {
  q->head = NULL;
  q->tail = NULL;
  q->size = 0;
}

void enqueue(Queue *q, Process *p) {
  p->next = NULL;

  if (q->tail == NULL) {
    q->head = p;
    q->tail = p;
  } else {
    q->tail->next = p;
    q->tail = p;
  }

  q->size++;
}

// Ready queue'daki process'lerden priority'si en yüksek olanı seçer.
// Eşitlik durumunda remaining time'a göre seçer.
Process *select_highest_priority(Queue *q) {
  if (q->head == NULL) {
    return NULL;
  }
  Process *selected = q->head;
  Process *current = q->head->next;

  while (current != NULL) {
    if (current->current_priority < selected->current_priority) {
      selected = current;

    } else if (current->current_priority == selected->current_priority) {
      if (current->remaining_time < selected->remaining_time) {
        selected = current;
      }
    }
    current = current->next;
  }
  return selected;
}

void remove_from_queue(Queue *q, Process *p) {
  if (q->head == NULL || p == NULL) {
    return;
  }

  if (q->head == p) {
    q->head = q->head->next;
    if (q->head == NULL) {
      q->tail = NULL;
    }
    q->size--;
    p->next = NULL;
    return;
  }

  Process *prev = q->head;
  while (prev->next != NULL && prev->next != p) {
    prev = prev->next;
  }

  if (prev->next == p) {
    prev->next = p->next;
    if (q->tail == p) {
      q->tail = prev;
    }
    q->size--;
    p->next = NULL;
  }
}

void parse_file(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (fp == NULL) {
    perror("Error opening input file");
    exit(1);
  }

  // Dosyadaki process(satır) sayısını bulur.
  num_processes = 0;
  char line[256];
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strlen(line) > 1) {
      num_processes++;
    }
  }

  all_processes = (Process *)malloc(num_processes * sizeof(Process));
  if (all_processes == NULL) {
    perror("Error allocating memory for processes");
    fclose(fp);
    exit(1);
  }

  rewind(fp);
  int i = 0;
  while (fgets(line, sizeof(line), fp) != NULL && i < num_processes) {
    if (strlen(line) <= 1)
      continue;

    Process *p = &all_processes[i];
    sscanf(line, "%d %d %d %d %d %d", &p->pid, &p->arrival_time,
           &p->cpu_exec_time, &p->interval_time, &p->io_time,
           &p->current_priority);

    p->remaining_time = p->cpu_exec_time;
    p->first_priority = p->current_priority;
    p->time_in_ready_queue = 0;
    p->last_ready_time = 0;
    p->current_burst_time = 0;
    p->io_completion_time = 0;
    p->next = NULL;

    i++;
  }
  fclose(fp);

  // Process'leri arrival time'a göre sıralar.
  for (int i = 0; i < num_processes - 1; i++) {
    for (int j = 0; j < num_processes - i - 1; j++) {
      if (all_processes[j].arrival_time > all_processes[j + 1].arrival_time) {
        Process temp = all_processes[j];
        all_processes[j] = all_processes[j + 1];
        all_processes[j + 1] = temp;
      }
    }
  }
}

// Current clock time'da gelen process'leri ready queue'ya ekler.
void add_arrivals_to_queue() {
  while (next_process_index < num_processes &&
         all_processes[next_process_index].arrival_time == current_clock) {

    Process *p = &all_processes[next_process_index];
    printf("[Clock: %d] PID %d arrived\n", current_clock, p->pid);
    fflush(stdout);

    pthread_mutex_lock(&queue_mutex);
    p->last_ready_time = current_clock;
    enqueue(&ready_queue, p);
    pthread_mutex_unlock(&queue_mutex);

    printf("[Clock: %d] PID %d moved to READY queue\n", current_clock, p->pid);
    fflush(stdout);

    next_process_index++;
  }
}

// Ready queue'daki process'lerin priority'sini 100ms'de bir azaltır.
void apply_aging() {
  pthread_mutex_lock(&queue_mutex);

  Process *current = ready_queue.head;
  while (current != NULL) {
    int time_in_queue = current_clock - current->last_ready_time;

    int aging = time_in_queue / 100;
    if (aging > 0) {
      int new_priority = current->first_priority - aging;
      if (new_priority < 0) {
        new_priority = 0;
      }

      current->current_priority = new_priority;
    }

    current = current->next;
  }

  pthread_mutex_unlock(&queue_mutex);
}

// Herhangi bir I/O tamamlandıysa 1 döner yoksa 0 döner.
int process_io_completions() {
  int processed = 0;

  Process *current = waiting_queue.head;
  Process *prev = NULL;

  while (current != NULL) {
    // Bu if bloğu I/O tamamlandıysa waiting queue'dan siler
    // ve ready queue'ya taşır.
    if (current->io_completion_time <= current_clock) {
      Process *next = current->next;

      if (prev == NULL) {
        waiting_queue.head = next;
      } else {
        prev->next = next;
      }

      if (waiting_queue.tail == current) {
        waiting_queue.tail = prev;
      }
      waiting_queue.size--;
      current->next = NULL;

      printf("[Clock: %d] PID %d finished I/O\n", current_clock, current->pid);
      fflush(stdout);

      current->last_ready_time = current_clock;
      enqueue(&ready_queue, current);
      printf("[Clock: %d] PID %d moved to READY queue\n", current_clock,
             current->pid);
      fflush(stdout);

      processed = 1;
      current = next;
    } else { // Eğer I/O tamamlanmamışsa sonraki process'e geçer.
      prev = current;
      current = current->next;
    }
  }

  return processed;
}

// Waiting queue'daki process'leri kontrol eder ve
// I/O tamamlandıysa ready queue'ya taşır.
void *io_manager_thread(void *arg) {
  (void)arg;

  while (!simulation_complete) {
    pthread_mutex_lock(&queue_mutex);
    if (waiting_queue.size > 0) {
      process_io_completions();
    }
    pthread_mutex_unlock(&queue_mutex);
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    perror("Wrong input format");
    exit(1);
  }

  init_queue(&ready_queue);
  init_queue(&waiting_queue);

  parse_file(argv[1]);

  pthread_t io_thread;
  if (pthread_create(&io_thread, NULL, io_manager_thread, NULL) != 0) {
    perror("Error creating I/O manager thread");
    exit(1);
  }

  int processes_terminated = 0;
  Process *running_process = NULL;
  int dispatch_time = 0;
  int burst_duration = 0;

  while (processes_terminated < num_processes) {
    add_arrivals_to_queue();

    apply_aging();

    // Process'in kullanım süresi bittiyse girer.
    if (running_process != NULL &&
        current_clock >= dispatch_time + burst_duration) {
          
      running_process->current_burst_time = 0;

      if (running_process->remaining_time <= 0) {
        printf("[Clock: %d] PID %d TERMINATED\n", current_clock,
               running_process->pid);
        fflush(stdout);

        processes_terminated++;
        running_process = NULL;
      } else {
        // Process, I/O için waiting queue'ya eklenir.
        pthread_mutex_lock(&queue_mutex);
        running_process->io_completion_time =
            current_clock + running_process->io_time;
        enqueue(&waiting_queue, running_process);
        pthread_mutex_unlock(&queue_mutex);

        printf("[Clock: %d] PID %d blocked for I/O for %d ms\n", current_clock,
               running_process->pid, running_process->io_time);
        fflush(stdout);

        running_process = NULL;
      }
    }

    // CPU boşsa en yüksek priority'li process'i seçer.
    if (running_process == NULL) {
      pthread_mutex_lock(&queue_mutex);
      running_process = select_highest_priority(&ready_queue);

      if (running_process != NULL) {
        remove_from_queue(&ready_queue, running_process);
        pthread_mutex_unlock(&queue_mutex);

        int remaining_interval = running_process->interval_time -
                                 running_process->current_burst_time;
        burst_duration = (remaining_interval < running_process->remaining_time)
                             ? remaining_interval
                             : running_process->remaining_time;

        printf("[Clock: %d] Scheduler dispatched PID %d (Pr: %d, Rm: %d) for "
               "%d ms "
               "burst\n",
               current_clock, running_process->pid,
               running_process->current_priority,
               running_process->remaining_time, burst_duration);
        fflush(stdout);

        dispatch_time = current_clock;
        running_process->remaining_time -= burst_duration;
        running_process->current_burst_time += burst_duration;

        if (running_process->current_burst_time >=
            running_process->interval_time) {
          running_process->current_burst_time = 0;
        }

        // Process dispatch edildiği için aging'i sıfırlar.
        running_process->first_priority = running_process->current_priority;
      } else {
        pthread_mutex_unlock(&queue_mutex);

        // CPU boşsa, ready queue'da boşsa, sonraki arrival'a geçerek optimize eder. 
        // Eğer I/O bekliyorsa normal increment yapılır.
        // Waiting queue'ya erişirken race condition'a düşmemek için mutex kullanır
        // ve değişkene kaydedilir (has_waiting_processes = (waiting_queue.size > 0)).
        pthread_mutex_lock(&queue_mutex);
        int has_waiting_processes = (waiting_queue.size > 0);
        pthread_mutex_unlock(&queue_mutex);

        if (!has_waiting_processes) {
          if (next_process_index < num_processes) {
            int next_arrival = all_processes[next_process_index].arrival_time;
            if (next_arrival > current_clock) {
              current_clock = next_arrival;
              continue; 
            }
          }
        }
      }
    }
    current_clock++;
    usleep(1000);
  }

  simulation_complete = 1;

  if (pthread_join(io_thread, NULL) != 0) {
    perror("Error joining I/O manager thread");
    exit(1);
  }

  free(all_processes);
  pthread_mutex_destroy(&queue_mutex);

  return 0;
}
