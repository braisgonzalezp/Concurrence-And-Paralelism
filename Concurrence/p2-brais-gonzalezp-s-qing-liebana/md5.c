#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <openssl/evp.h>

#include "options.h"
#include "queue.h"
#include "threads.h"


#define MAX_PATH 1024
#define BLOCK_SIZE (10*1024*1024)
#define MAX_LINE_LENGTH (MAX_PATH * 2)


struct file_md5 {
    char *file;
    unsigned char *hash;
    unsigned int hash_size;
};

typedef struct {
    char* dir;
    queue q;
}LectorArgs;

typedef struct {
    queue in_q;
    queue out_q;
    int*   finLector;
    int*   totalEntradas;
    mtx_t*  mtx_activos;
    int*   activos;
}CalculadorArgs;

typedef struct {
    char* dir;
    char* file;
    queue q;
}EscritorArgs;

typedef struct {
    queue q;
}CalculadorCheckArgs;

typedef EscritorArgs LectorCheckArgs;

void get_entries(char *dir, queue q);


void print_hash(struct file_md5 *md5) {
    for(int i = 0; i < md5->hash_size; i++) {
        printf("%02hhx", md5->hash[i]);
    }
}


void sum_file(struct file_md5 *md5) {
    EVP_MD_CTX *mdctx;
    int nbytes;
    FILE *fp;
    char *buf;

    if((fp = fopen(md5->file, "r")) == NULL) {
        printf("Could not open %s\n", md5->file);
        return;
    }

    buf = malloc(BLOCK_SIZE);
    const EVP_MD *md = EVP_get_digestbyname("md5");

    mdctx = EVP_MD_CTX_create();
    EVP_DigestInit_ex(mdctx, md, NULL);

    while((nbytes = fread(buf, 1, BLOCK_SIZE, fp)) >0)
        EVP_DigestUpdate(mdctx, buf, nbytes);

    md5->hash = malloc(EVP_MAX_MD_SIZE);
    EVP_DigestFinal_ex(mdctx, md5->hash, &md5->hash_size);

    EVP_MD_CTX_destroy(mdctx);
    free(buf);
    fclose(fp);
}


void recurse(char *entry, void *arg) {
    queue q = * (queue *) arg;
    struct stat st;

    stat(entry, &st);

    if(S_ISDIR(st.st_mode))
        get_entries(entry, q);
}


void add_files(char *entry, void *arg) {
    queue q = * (queue *) arg;
    struct stat st;

    stat(entry, &st);

    if(S_ISREG(st.st_mode)){
        q_insert(q, strdup(entry));
    }
}


void walk_dir(char *dir, void (*action)(char *entry, void *arg), void *arg) {
    DIR *d;
    struct dirent *ent;
    char full_path[MAX_PATH];

    if((d = opendir(dir)) == NULL) {
        printf("Could not open dir %s\n", dir);
        return;
    }

    while((ent = readdir(d)) != NULL) {
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") ==0)
            continue;

        snprintf(full_path, MAX_PATH, "%s/%s", dir, ent->d_name);
        action(full_path, arg);
    }

    closedir(d);
}

void get_entries(char *dir, queue q) {
    walk_dir(dir, add_files, &q);
    walk_dir(dir, recurse, &q);
}

//Función tipo hilo encargada de leer las entradas del directorio
int hiloLeeDir(void* ptrArgs){
    LectorArgs* args = ptrArgs;
    get_entries(args->dir, args->q);
    q_desbloqueo(args->q);//Ya no se agregarán más entradas a la cola de entrada
    return 0;
}

int hiloCalculaMd5(void* ptrArgs){
    CalculadorArgs* args = ptrArgs;
    char *ent;
    struct file_md5 *md5;
    while((ent = q_remove(args->in_q)) != NULL) {
        md5 = malloc(sizeof(struct file_md5));

        md5->file = ent;
        sum_file(md5);

        q_insert(args->out_q, md5);
    }
    mtx_lock(args->mtx_activos);
    if(--*args->activos == 0) q_desbloqueo(args->out_q);
    mtx_unlock(args->mtx_activos);

    return 0;
}

//Función tipo hilo encargada de escribir el resultado (los hashes) a un fichero
int hiloEscribeMd5(void* ptrArgs){
    EscritorArgs* args = ptrArgs;
    struct file_md5 *md5;
    FILE *out;
    int dirname_len;

    if((out = fopen(args->file, "w")) == NULL) {
        printf("Could not open output file\n");
        exit(0);
    }

    dirname_len = strlen(args->dir) + 1; // length of dir + /

    while((md5 = q_remove(args->q)) != NULL) {
        //printf("Paso 5\n");
        fprintf(out, "%s: ", md5->file + dirname_len);

        for(int i = 0; i < md5->hash_size; i++)
            fprintf(out, "%02hhx", md5->hash[i]);
        fprintf(out, "\n");

        free(md5->file);
        free(md5->hash);
        free(md5);
    }

    fclose(out);

    return 0;
}


void sum(struct options opt) {
    queue in_q, out_q;
    thrd_t idLectorEntradas;
    int calculadoresActivos;
    mtx_t mtx_calculadoresActivos;
    LectorArgs leeArgs;
    CalculadorArgs calculadorArgs;
    EscritorArgs escribeArgs;

    in_q  = q_create(opt.queue_size); //Ahora comprobamos cola de entrada de capacidad 1
    out_q = q_create(opt.queue_size);

    //Creación y arranque del HiloLector
    leeArgs.dir = opt.dir;
    leeArgs.q = in_q;
    thrd_create(&idLectorEntradas,hiloLeeDir,&leeArgs);

    //Creación y arranque de opt.num_threads HiloCalculador
    thrd_t* idsCalculadores = malloc(opt.num_threads*sizeof(thrd_t));
    calculadoresActivos = opt.num_threads;
    mtx_init(&mtx_calculadoresActivos,mtx_plain);
    calculadorArgs.out_q = out_q;
    calculadorArgs.in_q = in_q;
    calculadorArgs.activos = &calculadoresActivos;
    calculadorArgs.mtx_activos = &mtx_calculadoresActivos;
    for(int i=0; i<opt.num_threads; i++){
        thrd_create(&idsCalculadores[i],hiloCalculaMd5,&calculadorArgs);
    }

    //Creación y arranque del HiloEscritor
    thrd_t idEscritor;
    escribeArgs.dir = opt.dir;
    escribeArgs.file = opt.file;
    escribeArgs.q = out_q;
    thrd_create(&idEscritor,hiloEscribeMd5,&escribeArgs);


    //Esperamos la finalización del HiloLector
    thrd_join(idLectorEntradas,NULL);
    //Esperamos la finalización de pt.num_threads HilosCalculador
    for(int i=0; i<opt.num_threads; i++){
        thrd_join(idsCalculadores[i],NULL);
    }
    //Esperamos la finalización del HiloEscritor
    thrd_join(idEscritor,NULL);

    q_destroy(in_q);
    q_destroy(out_q);
    mtx_destroy(&mtx_calculadoresActivos);
}

void read_hash_file(char *file, char *dir, queue q) {
    FILE *fp;
    char line[MAX_LINE_LENGTH];
    char *file_name, *hash;
    int hash_len;

    if((fp = fopen(file, "r")) == NULL) {
        printf("Could not open %s : %s\n", file, strerror(errno));
        exit(0);
    }

    while(fgets(line, MAX_LINE_LENGTH, fp) != NULL) {
        char *field_break;
        struct file_md5 *md5 = malloc(sizeof(struct file_md5));

        if((field_break = strstr(line, ": ")) == NULL) {
            printf("Malformed md5 file\n");
            exit(0);
        }
        *field_break = '\0';

        file_name = line;
        hash      = field_break + 2;
        hash_len  = strlen(hash);

        md5->file      = malloc(strlen(file_name) + strlen(dir) + 2);
        sprintf(md5->file, "%s/%s", dir, file_name);
        md5->hash      = malloc(hash_len / 2);
        md5->hash_size = hash_len / 2;


        for(int i = 0; i < hash_len; i+=2)
            sscanf(hash + i, "%02hhx", &md5->hash[i / 2]);

        q_insert(q, md5);
    }

    fclose(fp);
}

//Función tipo hilo encargada de leer el fichero de hashes de check
int hiloLeer2(void* ptrArgs){
    LectorCheckArgs* args = ptrArgs;
    read_hash_file(args->file,args->dir,args->q);
    q_desbloqueo(args->q);//Ya no se agregarán más entradas a la cola de entrada
    return 0;
}


//Función tipo hilo encargada de calcular los hashes de check
int hiloCalculadorMd52(void* ptrArgs){
    //printf("Paso 1\n");
    CalculadorCheckArgs* args = ptrArgs;
    struct file_md5 *md5_in, md5_file;
    while((md5_in = q_remove(args->q)) != NULL) {
        md5_file.file = md5_in->file;
        //printf("File: %s\n", md5_file.file);
        sum_file(&md5_file);

        if(memcmp(md5_file.hash, md5_in->hash, md5_file.hash_size)!=0) {
            printf("File %s doesn't match.\nFound:    ", md5_file.file);
            print_hash(&md5_file);
            printf("\nExpected: ");
            print_hash(md5_in);
            printf("\n");
        }

        free(md5_file.hash);

        free(md5_in->file);
        free(md5_in->hash);
        free(md5_in);
    }
    return 0;
}



void check(struct options opt) {
    queue in_q;

    thrd_t idLectorHashes;
    LectorCheckArgs leeArgs;
    CalculadorCheckArgs escribeArgs;
    in_q = q_create(opt.queue_size);

    //read_hash_file(opt.file, opt.dir, in_q);
    //Creación y arranque del HiloLectorCheck
    leeArgs.dir = opt.dir;
    leeArgs.file = opt.file;
    leeArgs.q = in_q;
    thrd_create(&idLectorHashes,hiloLeer2,&leeArgs);
    //Creación y arranque de opt.num_threads HiloCalculadorCheck
    thrd_t* idsCalculadores = malloc(opt.num_threads*sizeof(thrd_t));
    escribeArgs.q = in_q;
    for(int i=0; i<opt.num_threads; i++){
        thrd_create(&idsCalculadores[i],hiloCalculadorMd52,&escribeArgs);
    }


    //Esperamos la finalización del HiloLectorSum
    thrd_join(idLectorHashes,NULL);
    //Esperamos la finalización de pt.num_threads HilosCalculadorCheck
    for(int i=0; i<opt.num_threads; i++){
        thrd_join(idsCalculadores[i],NULL);
    }

    q_destroy(in_q);
}

int main(int argc, char *argv[]) {

    struct options opt;

    opt.num_threads = 5;
    opt.queue_size  = 1000;
    opt.check       = true;
    opt.file        = NULL;
    opt.dir         = NULL;

    read_options (argc, argv, &opt);

    if(opt.check)
        check(opt);
    else
        sum(opt);

}