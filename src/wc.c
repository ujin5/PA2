#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdbool.h>
#ifdef __GNUC__
#define FORCE_INLINE __attribute__((always_inline)) inline
#else
#define FORCE_INLINE inline
#endif
static FORCE_INLINE uint32_t rotl32 ( uint32_t x, int8_t r )
{
  return (x << r) | (x >> (32 - r));
}

static FORCE_INLINE uint64_t rotl64 ( uint64_t x, int8_t r )
{
  return (x << r) | (x >> (64 - r));
}

#define	ROTL32(x,y)	rotl32(x,y)
#define ROTL64(x,y)	rotl64(x,y)

#define BIG_CONSTANT(x) (x##LLU)

//-----------------------------------------------------------------------------
// Block read - if your platform needs to do endian-swapping or can only
// handle aligned reads, do the conversion here

#define getblock(p, i) (p[i])

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

static FORCE_INLINE uint32_t fmix32 ( uint32_t h )
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;

  return h;
}

//----------

static FORCE_INLINE uint64_t fmix64 ( uint64_t k )
{
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
  k ^= k >> 33;

  return k;
}

//-----------------------------------------------------------------------------

void MurmurHash3_x86_32 ( const void * key, int len,
                          uint32_t seed, void * out )
{
  const uint8_t * data = (const uint8_t*)key;
  const int nblocks = len / 4;
  int i;

  uint32_t h1 = seed;

  uint32_t c1 = 0xcc9e2d51;
  uint32_t c2 = 0x1b873593;

  //----------
  // body

  const uint32_t * blocks = (const uint32_t *)(data + nblocks*4);

  for(i = -nblocks; i; i++)
  {
    uint32_t k1 = getblock(blocks,i);

    k1 *= c1;
    k1 = ROTL32(k1,15);
    k1 *= c2;
    
    h1 ^= k1;
    h1 = ROTL32(h1,13); 
    h1 = h1*5+0xe6546b64;
  }

  //----------
  // tail

  const uint8_t * tail = (const uint8_t*)(data + nblocks*4);

  uint32_t k1 = 0;

  switch(len & 3)
  {
  case 3: k1 ^= tail[2] << 16;
  case 2: k1 ^= tail[1] << 8;
  case 1: k1 ^= tail[0];
          k1 *= c1; k1 = ROTL32(k1,15); k1 *= c2; h1 ^= k1;
  };

  //----------
  // finalization

  h1 ^= len;

  h1 = fmix32(h1);

  *(uint32_t*)out = h1;
}

typedef struct _HashElem{
	char key[32];
	uint32_t hash;
} HashElem;

typedef struct _Map{
	HashElem hash;
	uint32_t value;
} Map;
Map * globalMap = NULL; 
Map * reduceMap = NULL;

pthread_mutex_t reduce_mutex;
pthread_mutex_t map_mutex;
int32_t shared_cur;
int32_t reduceN;
int flags = 0;
FILE * fp;
void * stringRemoveNonAlphaNum(char *str){
    int32_t i = 0;
    int32_t j = 0;
    char c;
    while ((c = str[i++]) != '\0')
    {
        if (isalnum(c))
        {
            str[j++] = c;
        }
    }
    str[j] = '\0';
   return str;
}
void allocate_Map() {
	globalMap = (Map *)mmap(0, sizeof(Map)* 0x1000000, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
	memset((void *)globalMap, 0x0, sizeof(Map)*0x100000);
  reduceMap = (Map *)mmap(0, sizeof(Map)* 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
	memset((void *)reduceMap, 0x0, sizeof(Map)*0x10000);
}
inline int32_t readWord(FILE * fp, char * buf){
  char c;
  char j = 0;
  while(!feof(fp)){
    char c = getc(fp);
    if (isalnum(c))
      buf[j++] = c;
  }
  buf[j] = '\0';
}
void do_Map(int32_t * nWord){
  //printf("START\n");
  char buf[32];
  for(int i = 0; ; i++){
    pthread_mutex_lock(&map_mutex);
    int32_t index = *nWord;
    readWord(fp, buf);
    printf("[%s]\n",buf);
    if(strlen(buf) == 0){
      pthread_mutex_unlock(&map_mutex);
      return;
    }
    strcpy(globalMap[index].hash.key, buf);
    MurmurHash3_x86_32(globalMap[index].hash.key, strlen(globalMap[index].hash.key), 0xdeadbeef, &globalMap[index].hash.hash);
    globalMap[index].value = 1;
    //printf("%d : %s / %08x / %08x\n", index, globalMap[index].hash.key, globalMap[index].hash.hash, globalMap[index].value);
    (*nWord)++;
    pthread_mutex_unlock(&map_mutex);
  }
}

void do_Reduce(int32_t nWord){
  int32_t max = nWord;
  while(1){
    pthread_mutex_lock(&reduce_mutex);
    if(shared_cur >= nWord){
      pthread_mutex_unlock(&reduce_mutex);
      return;
    }
    int32_t i = shared_cur;
    Map * tMap = &globalMap[i];
    int32_t dup = false;
    for(int32_t j = 0; j < reduceN; j++){
      if( tMap->hash.hash == reduceMap[j].hash.hash){
        memcpy(&reduceMap[j], tMap, sizeof(Map));
        reduceMap[j].value++;
        dup = true;
      }
    }
    if(!dup)
      memcpy(&reduceMap[reduceN++], tMap, sizeof(Map));
    shared_cur++;
    pthread_mutex_unlock(&reduce_mutex);
  }
	return;
}
int static compare (const Map* first, const Map* second)
{
    if (first->value > second->value)
        return -1;
    else if (first->value < second->value)
        return 1;
    else
        return 0;
}

int main(int argc, char ** argv){
	if (argc != 3) {
		fprintf(stderr, "%s: not enough input\n", argv[0]);
		exit(1);
	}
  pthread_t * threads = (pthread_t *)malloc(0x100 * sizeof(pthread_t));
  allocate_Map();
	fp = fopen(argv[1], "r");
	char buf[4096];
  pthread_mutex_init(&map_mutex, NULL);
  pthread_mutex_init(&reduce_mutex, NULL);

  int32_t index =0;
  int32_t nWord = 0;
  int32_t nThread = atoi(argv[2]);
  for(int32_t i = 0; i < nThread; i++)
    pthread_create(&threads[i], NULL, do_Map, &nWord);
  
  for(int32_t i = 0; i < nThread; i++)
    pthread_join(threads[i], NULL);
  
  for(int32_t i = 0; i < nThread; i++)
    pthread_create(&threads[i], NULL, do_Reduce, nWord);
  
  for(int32_t i = 0; i < nThread; i++)
    pthread_join(threads[i], NULL);
  qsort(reduceMap, reduceN, sizeof(Map), compare);
  for(int32_t i = 0; i < reduceN; i++)
    printf("%d : %s(%08x) = %d\n", i, reduceMap[i].hash.key, reduceMap[i].hash.hash, reduceMap[i].value);	
  /*
  while(fscanf(fp, "%s", buf) != EOF)
    printf("[%s]\n", buf);
  */
}
