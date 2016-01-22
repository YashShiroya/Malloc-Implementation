//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
// 
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "MyMalloc.h"

static pthread_mutex_t mutex;

const int ArenaSize = 2097152;
const int NumberOfFreeLists = 1;

// Header of an object. Used both when the object is allocated and freed
struct ObjectHeader {
    size_t _objectSize;         // Real size of the object.
    int _allocated;             // 1 = yes, 0 = no, 2 = sentinel
    struct ObjectHeader * _next;       // Points to the next object in the freelist (if free).
    struct ObjectHeader * _prev;       // Points to the previous object.
};

struct ObjectFooter {
    size_t _objectSize;
    int _allocated;
};

  //STATE of the allocator

  // Size of the heap
  static size_t _heapSize;

  // initial memory pool
  static void * _memStart;

  // number of chunks request from OS
  static int _numChunks;

  // True if heap has been initialized
  static int _initialized;

  // Verbose mode
  static int _verbose;

  // # malloc calls
  static int _mallocCalls;

  // # free calls
  static int _freeCalls;

  // # realloc calls
  static int _reallocCalls;
  
  // # realloc calls
  static int _callocCalls;

  // Free list is a sentinel
  static struct ObjectHeader _freeListSentinel; // Sentinel is used to simplify list operations
  static struct ObjectHeader *_freeList;


  //FUNCTIONS

  //Initializes the heap
  void initialize();

  // Allocates an object 
  void * allocateObject( size_t size );

  // Frees an object
  void freeObject( void * ptr );

  // Returns the size of an object
  size_t objectSize( void * ptr );

  // At exit handler
  void atExitHandler();

  //Prints the heap size and other information about the allocator
  void print();
  void print_list();

  // Gets memory from the OS
  void * getMemoryFromOS( size_t size );

  void increaseMallocCalls() { _mallocCalls++; }

  void increaseReallocCalls() { _reallocCalls++; }

  void increaseCallocCalls() { _callocCalls++; }

  void increaseFreeCalls() { _freeCalls++; }

extern void
atExitHandlerInC()
{
  atExitHandler();
}

void initialize()
{
  // Environment var VERBOSE prints stats at end and turns on debugging
  // Default is on
  _verbose = 1;
  const char * envverbose = getenv( "MALLOCVERBOSE" );
  if ( envverbose && !strcmp( envverbose, "NO") ) {
    _verbose = 0;
  }

  pthread_mutex_init(&mutex, NULL);
  void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)) );

  // In verbose mode register also printing statistics at exit
  atexit( atExitHandlerInC );

  //establish fence posts
  struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;							//Dummy Footer
  fencepost1->_allocated = 1;																//Allocated flag to true so that no venture beyond this point
  fencepost1->_objectSize = 123456789;														//Arbitrary size
  char * temp = 
      (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
  fencepost2->_allocated = 1;
  fencepost2->_objectSize = 123456789;
  fencepost2->_next = NULL;
  fencepost2->_prev = NULL;

  //initialize the list to point to the _mem
  temp = (char *) _mem + sizeof(struct ObjectFooter);
  struct ObjectHeader * currentHeader = (struct ObjectHeader *) temp;
  temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;
  _freeList = &_freeListSentinel;
  currentHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); //2MB
  currentHeader->_allocated = 0;
  currentHeader->_next = _freeList;
  currentHeader->_prev = _freeList;
  currentFooter->_allocated = 0;
  currentFooter->_objectSize = currentHeader->_objectSize;
  _freeList->_prev = currentHeader;
  _freeList->_next = currentHeader; 
  _freeList->_allocated = 2; // sentinel. no coalescing.
  _freeList->_objectSize = 0;
  _memStart = (char*) currentHeader;
}

void split(struct ObjectHeader * list_ptr, size_t roundedSize) {
		
		//Search for old footer position
		char * old_footer_position = (char*)list_ptr + list_ptr->_objectSize - sizeof(struct ObjectFooter);
		
		//Overwrite old footer	
		struct ObjectFooter * old_footer = (struct ObjectFooter*) old_footer_position;
	
		old_footer->_allocated = 0;
		old_footer->_objectSize = list_ptr->_objectSize - roundedSize;
	
		//Place new footer
		char * new_footer_position = (char*)list_ptr + roundedSize - sizeof(struct ObjectFooter);//sizeof(struct ObjectHeader) + raw_size;
		struct ObjectFooter * new_footer = (struct ObjectFooter*) new_footer_position;
				
		//Set new footer fields
		new_footer->_allocated = 1;
		new_footer->_objectSize = roundedSize;
		
		//Place new header
		char * new_header_position = (char*)list_ptr + roundedSize;
		struct ObjectHeader * new_header = (struct ObjectHeader*) new_header_position;
		
		//Set new header fields
		new_header->_next = list_ptr->_next;
		new_header->_prev = list_ptr->_prev; 
		new_header->_allocated = 0;
		new_header->_objectSize = list_ptr->_objectSize - roundedSize;
		
		//List changes
		list_ptr->_prev->_next = new_header;
		list_ptr->_next->_prev = new_header;		
		list_ptr->_allocated = 1;
		list_ptr->_objectSize = roundedSize;
}

void * allocateObject( size_t size )
{
  //Make sure that allocator is initialized
  if ( !_initialized ) {
    _initialized = 1;
    initialize();
  }

  // Add the ObjectHeader/Footer to the size and round the total size up to a multiple of
  // 8 bytes for alignment.
  	size_t roundedSize = (size + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter) + 7) & ~7;
  
	struct ObjectHeader * list_ptr = _freeList->_next;
	struct ObjectHeader * temp = list_ptr;
	int flag = -1;
	
	while(list_ptr != _freeList) {
		//Check if block is large enough for malloc call
		if(list_ptr->_objectSize > roundedSize) {
			flag = 0;
			size_t remainder = list_ptr->_objectSize - roundedSize - sizeof(struct ObjectHeader) - sizeof(struct ObjectFooter);  
			//Case 1: Split results in second block reuseable
			if(remainder > 8) flag = 1;	
			//Case 2: Split results in second block unuseable, so return entire block
			else flag = 2;

			break;	
		}
		list_ptr = list_ptr->_next;
	}
	
	//Case 1: Split results in second block reuseable	 
	if(flag == 1) {
	
		split(list_ptr, roundedSize);
		pthread_mutex_unlock(&mutex);
		return (void*) (list_ptr + 1);
		
	}
	//Case 2: Split results in second block unuseable, so return entire block
	else if(flag == 2) {
						
		char * new_footer_position = (char*)list_ptr + roundedSize - sizeof(struct ObjectFooter);//sizeof(struct ObjectHeader) + raw_size;
		struct ObjectFooter * new_footer = (struct ObjectFooter*) new_footer_position;
		new_footer->_allocated = 1;
		new_footer->_objectSize = list_ptr->_objectSize;
		temp->_allocated = 1;
		list_ptr->_prev->_next = list_ptr->_next;
		list_ptr->_next->_prev = list_ptr->_prev;
		pthread_mutex_unlock(&mutex);
		return (void*) (temp + 1);
		
	}
	//Case 3: Request 2MB chunk
	else if(flag == -1) {
			
		  void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)));

		  //establish fence posts
		  struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;							//Dummy Footer
		  fencepost1->_allocated = 1;																//Allocated flag to true so that no venture beyond this point
		  fencepost1->_objectSize = 123456789;														//Arbitrary size
		  char * temp = 
			  (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;
		  struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
		  fencepost2->_allocated = 1;
		  fencepost2->_objectSize = 123456789;
		  fencepost2->_next = NULL;
		  fencepost2->_prev = NULL;

		  //initialize the list to point to the _mem
		  temp = (char *) _mem + sizeof(struct ObjectFooter);
		  struct ObjectHeader * currentHeader = (struct ObjectHeader *) temp;
		  temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
		  struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;

		  currentHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); //2MB
		  currentHeader->_allocated = 0;
		  
		  //Add new 2MB chunk to previous list
		  currentHeader->_next = list_ptr;
		  currentHeader->_prev = list_ptr->_prev;
		  currentFooter->_allocated = 0;
		  currentFooter->_objectSize = currentHeader->_objectSize;
		  
		  list_ptr->_prev->_next = currentHeader;
		  list_ptr->_prev = currentHeader;
		  list_ptr = list_ptr->_prev;
		  
		  //Split
		  split(list_ptr, roundedSize);
		  pthread_mutex_unlock(&mutex);
		  return (void*) (list_ptr + 1);
				  
	}
	
}



void freeObject( void * ptr )
{
  // Add your code here
  //Set ptr to head of memory block
  char * mover_head_current = (char*) ptr - sizeof(struct ObjectHeader);
  struct ObjectHeader * current_header = (struct ObjectHeader*) mover_head_current;
  
  char * mover_foot_current = mover_head_current + current_header->_objectSize - sizeof(struct ObjectFooter);
  struct ObjectFooter * current_footer = (struct ObjectFooter*) mover_foot_current;
  
  char * mover_left_foot = (char*) ptr - sizeof(struct ObjectHeader) - sizeof(struct ObjectFooter);
  struct ObjectFooter * left_footer = (struct ObjectFooter*) mover_left_foot;
  
  char * mover_left_head = mover_left_foot + sizeof(struct ObjectFooter) - left_footer->_objectSize;
  struct ObjectHeader * left_header = (struct ObjectHeader*) mover_left_head;
  
  char * mover_right_head = (char*) ptr - sizeof(struct ObjectHeader) + current_header->_objectSize;
  struct ObjectHeader * right_header = (struct ObjectHeader*) mover_right_head;
  
  char * mover_right_foot = mover_right_head + right_header->_objectSize - sizeof(struct ObjectFooter);
  struct ObjectFooter * right_footer = (struct ObjectFooter*) mover_right_head;
  
  int coal_left = 0, coal_right = 0, coal_both = -1;


	if(left_footer->_allocated == 0) coal_left = 1;
	if(right_header->_allocated == 0) coal_right = 1;
	
	struct ObjectHeader * pointer = current_header;
	
	coal_both = coal_left + coal_right;
	
    /*if(coal_both == 0) {
    	current_header->_allocated = 0;
    	current_footer->_allocated = 0;
    }*/
    if(coal_both == 1) {
		if(coal_right == 1) {
			current_header->_objectSize = current_header->_objectSize + right_header->_objectSize;
			current_header->_allocated = 0;
			
			current_header->_next = right_header->_next;
			current_header->_prev = right_header->_prev;
			
			right_header->_prev->_next = current_header;
			//right_header->_next->_prev = current_header;
			right_footer->_allocated = 0;
			right_footer->_objectSize = current_header->_objectSize;
			pointer = current_header;
			
		}
	}
		/*else if(coal_left == 1) {
			left_header->_allocated = 0;
			left_header->_objectSize = left_header->_objectSize + current_header->_objectSize;
			current_footer->_allocated = 0;
			current_footer->_objectSize = left_header->_objectSize;
			pointer = left_header;
		} 
	}
	else if(coal_both == 2) {
		left_header->_allocated = 0;
		left_header->_objectSize = left_header->_objectSize + current_header->_objectSize + right_header->_objectSize;
		
		right_footer->_allocated = 0;
		right_footer->_objectSize = left_header->_objectSize;
		
		left_header->_next = left_header->_next->_next;
		right_header->_next->_prev = left_header;
		pointer = left_header;
			/*current_header->_objectSize = current_header->_objectSize + right_header->_objectSize;
			current_header->_allocated = 0;
			
			current_header->_next = right_header->_next;
			current_header->_prev = right_header->_prev;
			
			right_header->_prev->_next = current_header;
			right_footer->_allocated = 0;
			right_footer->_objectSize = current_header->_objectSize;
			pointer = current_header;
	}*/
	
	struct ObjectHeader * p = _freeList->_next;
	
	
	while(p != _freeList) {
		if(pointer < p) {
			pointer->_allocated = 0;
			pointer->_next = p;
			pointer->_prev = p->_prev;
			p->_prev->_next = pointer;
			p->_prev = pointer;
			break;
		}
		p = p->_next;
	}

	
  return;

}

size_t objectSize( void * ptr )
{
  // Return the size of the object pointed by ptr. We assume that ptr is a valid obejct.
  struct ObjectHeader * o =
    (struct ObjectHeader *) ( (char *) ptr - sizeof(struct ObjectHeader) );

  // Subtract the size of the header
  return o->_objectSize;
}

void print()
{
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", _heapSize );
  printf("# mallocs:\t%d\n", _mallocCalls );
  printf("# reallocs:\t%d\n", _reallocCalls );
  printf("# callocs:\t%d\n", _callocCalls );
  printf("# frees:\t%d\n", _freeCalls );

  printf("\n-------------------\n");
}

void print_list()
{
  printf("FreeList: ");
  if ( !_initialized ) {
    _initialized = 1;
    initialize();
  }
  struct ObjectHeader * ptr = _freeList->_next;
  while(ptr != _freeList){
      long offset = (long)ptr - (long)_memStart;
      printf("[offset:%ld,size:%zd]",offset,ptr->_objectSize);
      ptr = ptr->_next;
      if(ptr != NULL){
          printf("->");
      }
  }
  printf("\n");
}

void * getMemoryFromOS( size_t size )
{
  // Use sbrk() to get memory from OS
  _heapSize += size;
 
  void * _mem = sbrk( size );

  if(!_initialized){
      _memStart = _mem;
  }

  _numChunks++;

  return _mem;
}

void atExitHandler()
{
  // Print statistics when exit
  if ( _verbose ) {
    print();
  }
}

//
// C interface
//

extern void *
malloc(size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseMallocCalls();
  
  return allocateObject( size );
}

extern void
free(void *ptr)
{
  pthread_mutex_lock(&mutex);
  increaseFreeCalls();
  
  if ( ptr == 0 ) {
    // No object to free
    pthread_mutex_unlock(&mutex);
    return;
  }
  
  freeObject( ptr );
}

extern void *
realloc(void *ptr, size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseReallocCalls();
    
  // Allocate new object
  void * newptr = allocateObject( size );

  // Copy old object only if ptr != 0
  if ( ptr != 0 ) {
    
    // copy only the minimum number of bytes
    size_t sizeToCopy =  objectSize( ptr );
    if ( sizeToCopy > size ) {
      sizeToCopy = size;
    }
    
    memcpy( newptr, ptr, sizeToCopy );

    //Free old object
    freeObject( ptr );
  }

  return newptr;
}

extern void *
calloc(size_t nelem, size_t elsize)
{
  pthread_mutex_lock(&mutex);
  increaseCallocCalls();
    
  // calloc allocates and initializes
  size_t size = nelem * elsize;

  void * ptr = allocateObject( size );

  if ( ptr ) {
    // No error
    // Initialize chunk with 0s
    memset( ptr, 0, size );
  }

  return ptr;
}

