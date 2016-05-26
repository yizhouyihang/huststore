#include "kv_array.h"
#include "key_hash.h"
#include "../../hustdb.h"
#include "../md5db/bucket.h"
#include "../../../network/hustdb_utils.h"

void kv_array_t::kill_me ( )
{
    delete this;
}

kv_array_t::kv_array_t ( )
: m_config ( )
, m_files ( )
, m_hash ( NULL )
, m_get_buffers ( )
, m_ok ( false )
{
}

kv_array_t::~ kv_array_t ( )
{
    close ();
}

i_kv_t * kv_array_t::create_file ( )
{
    i_kv_t * p = create_kv ( );
    if ( NULL == p )
    {
        LOG_ERROR ( "[kv_array]create_kv failed" );
        return NULL;
    }

    return p;
}

bool kv_array_t::open ( )
{
    if ( NULL == m_hash )
    {
        try
        {
            m_hash = new key_hash_t ();
        }
        catch ( ... )
        {
            LOG_ERROR ( "[kv_array]bad_alloc" );
            return false;
        }
    }

    if ( ! m_config.open ( HUSTDB_CONFIG, 0, DB_DATA_ROOT, * m_hash, * this ) )
    {
        LOG_ERROR ( "[kv_array]m_config.open failed" );
        return false;
    }

    return open ( m_config );
}

bool kv_array_t::open ( config_t & config )
{
    try
    {
        int count = (( hustdb_t * ) G_APPTOOL->get_hustdb ())->get_worker_count () + 1;
        m_get_buffers.resize ( count );
        for ( int i = 0; i < count; ++ i )
        {
            std::string & s = m_get_buffers[ i ].value;

            s.resize ( RESERVE_BYTES_FOR_RSP_BUFER );
            memset ( & s[ 0 ], 0, RESERVE_BYTES_FOR_RSP_BUFER );

            s.resize ( 0 );
        }
    }
    catch ( ... )
    {
        LOG_ERROR ( "[kv_array]bad_alloc" );
        return false;
    }

    int count = config.get_max_file_count ();

    try
    {
        m_files.resize ( count, NULL );
    }
    catch ( ... )
    {
        LOG_ERROR ( "[kv_array]bad_alloc" );
        return false;
    }

    int i;
    for ( i = 0; i < count; ++ i )
    {
        const char * path = config.get_file_path ( i );
        if ( NULL == path )
        {
            LOG_ERROR ( "[kv_array]config.get_file_path( %d ) failed", i );
            return false;
        }
        if ( '\0' == * path )
        {
            continue;
        }

        i_kv_t * o = NULL;
        try
        {
            o = create_file ();
        }
        catch ( ... )
        {
            LOG_ERROR ( "[kv_array]bad_alloc" );
            return false;
        }
        if ( NULL == o )
        {
            LOG_ERROR ( "[kv_array]create_file() return NULL" );
            return false;
        }
        const kv_config_t & kv_cfg = config.get_kv_config ();
        if ( ! o->open ( path, kv_cfg, i ) )
        {
            LOG_ERROR ( "[kv_array]o->open( %s ) failed", path );
            o->kill_me ();
            return false;
        }

        LOG_DEBUG ( "[kv_array][i=%u][p=%p][file_id=%u] file opened[path=%s]", i, o, o->get_id (), o->get_path () );

        m_files[ i ] = o;
    }

    m_ok = true;
    
    return true;
}

void kv_array_t::close ( )
{
    m_ok = false;

    if ( ! m_files.empty () )
    {
        array_t::iterator e = m_files.end ();
        for ( array_t::iterator i = m_files.begin (); i != e; ++ i )
        {
            i_kv_t * o = * i;
            * i = NULL;
            if ( o )
            {
                int file_id = o->get_id ();
                std::string path;
                try
                {
                    path = o->get_path ();
                }
                catch ( ... )
                {
                }
                LOG_INFO ( "[kv_array][file_id=%u]begin close data_file[path=%s]",
                          file_id, path.c_str () );
                o->kill_me ();
                LOG_INFO ( "[kv_array][file_id=%u]end close data_file[path=%s]",
                          file_id, path.c_str () );
            }
        }
        m_files.resize ( 0 );
    }

    m_config.close ();

    if ( m_hash )
    {
        delete m_hash;
        m_hash = NULL;
    }
}

int kv_array_t::flush ( )
{
    //TODO:
    return - 1;
}

i_kv_t * kv_array_t::get_file ( unsigned int file_id )
{
    if ( unlikely ( ( unsigned int ) - 1 == file_id || file_id >= ( unsigned int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array]file_id %d >= m_files.size() %d", ( int ) file_id, ( int ) m_files.size () );
        return NULL;
    }

    return m_files[ file_id ];
}

unsigned int kv_array_t::file_count ( )
{
    return ( unsigned int ) m_files.size ();
}

int kv_array_t::get_from_md5db (
                                 const md5db::block_id_t &   block_id,
                                 uint32_t                    file_id,
                                 const char *                table,
                                 size_t                      table_len,
                                 std::string * &             rsp,
                                 item_ctxt_t * &              ctxt
                                 )
{
    assert ( ctxt );
    rsp = NULL;

    if ( unlikely ( ! m_ok ) )
    {
        LOG_ERROR ( "[kv_array]not ready" );
        return EINVAL;
    }

    if ( unlikely ( file_id >= ( int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array][local=%d][files=%d]invalid local_id",
                   file_id,
                   ( int ) m_files.size () );
        return EINVAL;
    }

    i_kv_t * kv = m_files[ file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]file is NULL", file_id );
        return EFAULT;
    }

    int r;
    if ( table_len <= 0 )
    {
        r = kv->get ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ), ctxt->value );
    }
    else
    {
        ctxt->key.append ( table, table_len );
        ctxt->key.append ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ) );

        r = kv->get ( ctxt->key.c_str (), ctxt->key.size (), ctxt->value );
    }

    if ( unlikely ( 0 != r && ENOENT != r ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]get return %d", file_id, r );
    }
    else
    {
        rsp = & ctxt->value;
    }

    return r;
}

int kv_array_t::put_from_md5db (
                                 const md5db::block_id_t &   block_id,
                                 uint32_t                    file_id,
                                 const char *                table,
                                 size_t                      table_len,
                                 const char *                val,
                                 size_t                      val_len,
                                 item_ctxt_t * &             ctxt
                                 )
{
    assert ( ctxt );

    if ( unlikely ( ! m_ok ) )
    {
        LOG_ERROR ( "[kv_array]not ready" );
        return EINVAL;
    }

    if ( unlikely ( file_id >= ( int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array][local=%d][files=%d]invalid local_id",
                   file_id, ( int ) m_files.size () );
        return EINVAL;
    }

    i_kv_t * kv = m_files[ file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]file is NULL" );
        return EFAULT;
    }

    int r;
    if ( table_len <= 0 )
    {
        r = kv->put ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ), val, val_len );
    }
    else
    {
        ctxt->key.append ( table, table_len );
        ctxt->key.append ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ) );

        r = kv->put ( ctxt->key.c_str (), ctxt->key.size (), val, val_len );
    }

    if ( unlikely ( 0 != r ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]put return %d", file_id, r );
    }

    return r;
}

int kv_array_t::hash_with_md5db (
                                  const char *        key,
                                  size_t              key_len,
                                  conn_ctxt_t         conn,
                                  item_ctxt_t * &     ctxt
                                  )
{
    assert ( key );
    ctxt = & m_get_buffers[ conn.worker_id ];
    ctxt->reset ();

    ctxt->user_file_id = G_APPTOOL->hust_hash_key ( key, key_len ) % m_hash->get_user_file_count () ;
    ctxt->inner_file_id = ctxt->user_file_id;

    return 0;
}

int kv_array_t::del_from_md5db (
                                 const md5db::block_id_t &   block_id,
                                 uint32_t                    file_id,
                                 const char *                table,
                                 size_t                      table_len,
                                 item_ctxt_t * &             ctxt
                                 )
{
    assert ( ctxt );

    if ( unlikely ( ! m_ok ) )
    {
        LOG_ERROR ( "[kv_array]not ready" );
        return EINVAL;
    }

    if ( unlikely ( file_id >= ( int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array][local=%d][files=%d]invalid local_id",
                   file_id,
                   ( int ) m_files.size () );
        return EINVAL;
    }

    i_kv_t * kv = m_files[ file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]file is NULL", file_id );
        return EFAULT;
    }

    int r;
    if ( table_len <= 0 )
    {
        r = kv->del ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ) );
    }
    else
    {
        ctxt->key.append ( table, table_len );
        ctxt->key.append ( ( const char * ) & block_id, sizeof ( md5db::block_id_t ) );

        r = kv->del ( ctxt->key.c_str (), ctxt->key.size () );
    }

    if ( unlikely ( 0 != r && ENOENT != r ) )
    {
        LOG_ERROR ( "[kv_array][file_id=%u]del return %d", file_id, r );
    }

    return r;
}

void kv_array_t::hash ( const char * key, size_t key_len, conn_ctxt_t conn, item_ctxt_t * & ctxt )
{
    ctxt = & m_get_buffers[ conn.worker_id ];
    ctxt->value.resize ( 0 );
    ctxt->user_file_id = m_hash->hash_with_cluster ( key,
                                                    ( int ) key_len,
                                                    ctxt->hash_other_servers,
                                                    ctxt->inner_file_id );
}

void kv_array_t::info ( std::stringstream & ss )
{
    unsigned int count = file_count ();
    
    for ( unsigned int i = 0; i < count; ++ i )
    {
        i_kv_t * p = get_file ( i );
        
        if ( p )
        {
            p->info ( ss );
            
            if ( i < count - 1 )
            {
                ss << ",";
            }
        }
    }
}

typedef struct
{
    const void *    key;
    const void *    data;
    size_t          len;
    uint32_t        numeric;
    bool            base64;
} export_item_t;

int kv_array_t::export_db (
                            int                         file_id,
                            const char *                path,
                            export_record_callback_t    callback,
                            void *                      callback_param
                            )
{
    int                 r                     = 0;
    uint32_t            offset                = 0;
    uint32_t            size                  = 0;
    uint16_t            start                 = 0;
    uint16_t            end                   = 0;
    uint32_t            i                     = 0;
    uint32_t            version               = 0;
    uint32_t            ttl                   = 0;
    size_t              key_len               = 0;
    const char *        key                   = NULL;
    size_t              val_len               = 0;
    const char *        val                   = NULL;
    size_t              prefix_tl             = 0;
    size_t              table_len             = 0;
    const char *        table                 = NULL;
    size_t              type_len              = 0;
    char                type[ 2 ]             = { };
    i_iterator_t *      it                    = NULL;
    FILE *              i_fp                  = NULL;
    FILE *              d_fp                  = NULL;
    uint64_t            index_ptr             = 0;
    uint16_t            path_len              = strlen ( path );
    bool                noval                 = true;
    bool                ignore_this_record    = false;
    bool                break_the_loop        = false;
    bool                is_seek               = false;
    bool                val_b64               = true;
    const char          flag_k[]              = "k";
    const char          flag_kv[]             = "kv";
    const char *        noval_flag            = NULL;
    char                i_ph[ 256 ]           = { };
    char                d_ph[ 256 ]           = { };
    char                json_numeric[ 32 ]    = { };
    c_str_t             base64_src;
    c_str_t             base64_dst;
    std::string         base64_item;
    std::string         json_item;
    std::string         content;

    if ( unlikely ( file_id >= ( int ) m_files.size () || ! path || ! * path ) )
    {
        LOG_ERROR ( "[kv_array][export_db][local=%d][files=%d][path is null]", file_id, ( int ) m_files.size () );
        return EFAULT;
    }
    
    base64_item.reserve ( RESERVE_BYTES_FOR_RSP_BUFER );
    json_item.reserve ( RESERVE_BYTES_FOR_RSP_BUFER );

    i_kv_t * kv = m_files[ file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][export_db][local=%u]file is NULL", file_id );
        return EFAULT;
    }

    if ( strncmp ( path, EXPORT_DB_ALL, sizeof ( EXPORT_DB_ALL ) - 1 ) != 0 )
    {
        is_seek = true;
    }

    struct export_cb_param_t * cb_pm = ( struct export_cb_param_t * ) callback_param;
    start      = cb_pm->start;
    end        = cb_pm->end;
    offset     = cb_pm->offset;
    size       = cb_pm->size;
    noval      = cb_pm->noval;

    noval_flag = noval ? flag_k : flag_kv;

    do
    {

        sprintf ( i_ph, "./EXPORT/%s%d[%d-%d].%s", path, file_id, start, end, noval_flag );
        G_APPTOOL->path_to_os ( i_ph );

        sprintf ( d_ph, "./EXPORT/%s%d[%d-%d].%s.data", path, file_id, start, end, noval_flag );
        G_APPTOOL->path_to_os ( d_ph );

        i_fp = fopen ( i_ph, "wb" );
        if ( ! i_fp )
        {
            LOG_ERROR ( "[kv_array][export_db]fopen[%s] failed", i_ph );
            r = EFAULT;
            break;
        }

        d_fp = fopen ( d_ph, "wb" );
        if ( ! d_fp )
        {
            LOG_ERROR ( "[kv_array][export_db]fopen[%s] failed", d_ph );
            r = EFAULT;
            break;
        }

        uint32_t total = offset + size;
        if ( total == 0 )
        {
            offset    = 0;
            total     = 0xFFFFFFFF;
        }

        it = kv->iterator ();
        if ( NULL == it )
        {
            LOG_ERROR ( "[kv_array][export_db]iterator() failed" );
            r = EFAULT;
            break;
        }

        if ( is_seek )
        {
            it->seek ( path, path_len );
        }
        else
        {
            it->seek_first ();
        }

        for ( i = 0; it->valid () && i < total; it->next (), i ++ )
        {
            if ( i < offset )
            {
                continue;
            }

            version    = 0;
            ttl        = 0;
            key        = it->key ( & key_len );
            val        = it->value ( & val_len );

            if ( key_len > sizeof ( md5db::block_id_t ) )
            {
                prefix_tl = key_len - sizeof ( md5db::block_id_t ) - 1;
                table     = key;

                if ( key_len > ZSET_SCORE_LEN + sizeof ( md5db::block_id_t ) + 1 &&
                     key [ key_len - ZSET_SCORE_LEN - sizeof ( md5db::block_id_t ) - 1 ] == ZSET_TB
                     )
                {
                    val_b64   = false;
                    table_len = key_len - ZSET_SCORE_LEN - sizeof ( md5db::block_id_t ) - 1;
                }
                else
                {
                    val_b64   = true;
                    table_len = prefix_tl;
                }

                if ( ! is_seek )
                {
                    if ( key [ table_len ] == ZSET_IN )
                    {
                        continue;
                    }

                    type[ 0 ] = hustdb_t::real_table_type ( key [ table_len ] );
                    type_len  = 1;
                }
                else
                {
                    table_len = 0;
                    type_len  = 0;
                }
            }
            else
            {
                prefix_tl = 0;
                table_len = 0;
                table     = NULL;
                type_len  = 0;
                val_b64   = true;
            }

            if ( unlikely ( is_seek && strncmp ( key, path, path_len ) != 0 ) )
            {
                break;
            }

            if ( callback )
            {
                callback ( callback_param,
                          key,
                          key_len,
                          val,
                          val_len,
                          table,
                          prefix_tl,
                          version,
                          ttl,
                          content,
                          & ignore_this_record,
                          & break_the_loop );
            }

            if ( unlikely ( ignore_this_record || version <= 0 ) )
            {
                continue;
            }
            
            if ( sizeof ( uint64_t ) != fwrite ( & index_ptr, 1, sizeof ( uint64_t ), i_fp ) )
            {
                LOG_ERROR ( "[kv_array][export_db]fwrite index_ptr failed" );
                r = EFAULT;
                break;
            }

            export_item_t items[] = {
                { "key",        key,        key_len,      0,          true     },
                { "val",        val,        val_len,      0,          val_b64  },
                { "ver",        NULL,       0,            version,    false    },
                { "ttl",        NULL,       0,            ttl,        false    },
                { "tb",         table,      table_len,    0,          false    },
                { "ty",         type,       type_len,     0,          false    }
            };
            size_t size = sizeof (items ) / sizeof (export_item_t );

            json_item.resize ( 0 );
            json_item += "{";
            
            for ( size_t j = 0; j < size; ++ j )
            {
                if ( items [ j ].numeric <= 0 && ( ! items [ j ].data || items [ j ].len <= 0 ) )
                {
                    continue;
                }
                
                json_item += "\"";
                json_item += ( char * ) items [ j ].key;
                json_item += "\":";

                if ( items [ j ].base64 )
                {
                    base64_src.len = items [ j ].len;
                    base64_src.data = ( char * ) items [ j ].data;
                    
                    base64_dst.len = c_base64_encoded_length(base64_src.len);
                    base64_item.resize ( base64_dst.len );
                    base64_dst.data = ( char * ) base64_item.c_str ();
                    
                    hustdb_base64_encode ( & base64_src, & base64_dst );
                    
                    json_item += "\"";
                    json_item.append ( base64_dst.data, base64_dst.len );
                    json_item += "\"";
                }
                else if ( items [ j ].numeric > 0 )
                {
                    memset ( json_numeric, 0, sizeof ( json_numeric ) );
                    sprintf ( json_numeric, "%u", items [ j ].numeric );
                    
                    json_item += json_numeric;
                }
                else
                {
                    json_item += "\"";
                    json_item.append ( ( char * ) items [ j ].data, items [ j ].len );
                    json_item += "\"";
                }
                
                json_item += ",";
            }
            
            if ( json_item.size () > 2 )
            {
                json_item.erase ( json_item.end () - 1, json_item.end () );
            }
            json_item += "}";

            if ( json_item.size () != fwrite ( json_item.c_str (), 1, json_item.size (), d_fp ) )
            {
                LOG_ERROR ( "[kv_array][export_db]fwrite item [len=%d] failed", json_item.size () );
                r = EFAULT;
                break_the_loop = true;
                break;
            }

            index_ptr += json_item.size ();

            if ( unlikely ( break_the_loop ) )
            {
                break;
            }
        }

        r = 0;

    }
    while ( 0 );

    if ( i_fp )
    {
        fclose ( i_fp );
        i_fp = NULL;
    }

    if ( d_fp )
    {
        fclose ( d_fp );
        d_fp = NULL;
    }

    if ( it )
    {
        it->kill_me ();
        it = NULL;
    }

    return r;
}

int kv_array_t::export_db_mem (
                                conn_ctxt_t                 conn,
                                std::string * &             rsp,
                                item_ctxt_t * &             ctxt,
                                export_record_callback_t    callback,
                                void *                      callback_param
                                )
{
    rsp = NULL;

    assert ( ctxt );

    int                 r                     = 0;
    uint32_t            offset                = 0;
    uint32_t            size                  = 0;
    uint16_t            start                 = 0;
    uint16_t            end                   = 0;
    int                 file_id               = 0;
    uint32_t            i                     = 0;
    uint32_t            real_size             = 0;
    size_t              key_len               = 0;
    const char *        key                   = NULL;
    size_t              val_len               = 0;
    const char *        val                   = NULL;
    size_t              prefix_tl             = 0;
    size_t              table_len             = 0;
    const char *        table                 = NULL;
    size_t              type_len              = 0;
    uint64_t            min                   = 0;
    uint64_t            max                   = 0;
    char                type[ 2 ]             = { };
    bool                noval                 = true;
    bool                ignore_this_record    = false;
    bool                break_the_loop        = false;
    bool                is_seek               = false;
    bool                val_b64               = true;
    const char          flag_k[]              = "k";
    const char          flag_kv[]             = "kv";
    const char *        noval_flag            = NULL;

    if ( unlikely ( ! m_ok ) )
    {
        LOG_ERROR ( "[kv_array][export_db_mem]not ready" );
        return EFAULT;
    }

    struct export_cb_param_t * cb_pm = ( struct export_cb_param_t * ) callback_param;
    start      = cb_pm->start;
    end        = cb_pm->end;
    offset     = cb_pm->offset;
    size       = cb_pm->size;
    noval      = cb_pm->noval;
    min        = cb_pm->min;
    max        = cb_pm->max;

    noval_flag = noval ? flag_k : flag_kv;

    file_id = cb_pm->file_id >= 0 ? cb_pm->file_id : ctxt->inner_file_id;

    if ( unlikely ( file_id >= ( int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array][export_db_mem][local=%d][files=%d]invalid local_id", file_id, ( int ) m_files.size () );
        return EFAULT;
    }

    i_kv_t * kv = m_files[ file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][export_db_mem][file_id=%u]file is NULL", file_id );
        return EFAULT;
    }

    if ( ctxt->key.empty () )
    {
        LOG_ERROR ( "[kv_array][export_db_mem]seek table empty" );
        return EFAULT;
    }

    if ( strncmp ( ctxt->key.c_str (), EXPORT_DB_ALL, sizeof ( EXPORT_DB_ALL ) - 1 ) != 0 )
    {
        is_seek = true;
    }

    ctxt->value.resize ( 0 );
    ctxt->value += "[";
    
    if ( cb_pm->async )
    {
        FILE *          i_fp                  = NULL;
        FILE *          d_fp                  = NULL;
        uint64_t        index_seek            = 0;
        uint64_t        data_seek             = 0;
        uint64_t        data_seek2            = 0;
        uint64_t        index_total           = 0;
        uint64_t        data_total            = 0;
        char            i_ph[ 256 ]           = { };
        char            d_ph[ 256 ]           = { };
        uint32_t        json_item_len         = 0;
        std::string     json_item;

        do
        {

            json_item.reserve ( RESERVE_BYTES_FOR_RSP_BUFER );

            sprintf ( i_ph, "./EXPORT/%s%d[%d-%d].%s", ctxt->key.c_str (), file_id, start, end, noval_flag );
            G_APPTOOL->path_to_os ( i_ph );

            sprintf ( d_ph, "./EXPORT/%s%d[%d-%d].%s.data", ctxt->key.c_str (), file_id, start, end, noval_flag );
            G_APPTOOL->path_to_os ( d_ph );

            i_fp = fopen ( i_ph, "rb+" );
            if ( ! i_fp )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]fopen[%s] failed", i_ph );
                r = EFAULT;
                break;
            }

            d_fp = fopen ( d_ph, "rb+" );
            if ( ! d_fp )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]fopen[%s] failed", d_ph );
                r = EFAULT;
                break;
            }

            fseek ( i_fp, 0L, SEEK_END );
            index_total = ftell ( i_fp );
            fseek ( i_fp, 0L, SEEK_SET );

            fseek ( d_fp, 0L, SEEK_END );
            data_total = ftell ( d_fp );
            fseek ( d_fp, 0L, SEEK_SET );

            cb_pm->total = index_total / 8;

            index_seek = offset * sizeof ( uint64_t );

            if ( index_seek >= index_total || fseek ( i_fp, index_seek, SEEK_SET ) != 0 )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]fseek index failed" );
                r = EFAULT;
                break;
            }

            if ( fread ( & data_seek, sizeof ( uint64_t ), 1, i_fp) != 1 )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]fread data_seek failed" );
                r = EFAULT;
                break;
            }

            if ( fseek ( d_fp, data_seek, SEEK_SET ) != 0 )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]fseek data failed" );
                r = EFAULT;
                break;
            }

            for ( i = 0; i < size; i ++ )
            {
                if ( fread ( & data_seek2, sizeof ( uint64_t ), 1, i_fp) != 1 )
                {
                    if ( index_seek + sizeof ( uint64_t ) == index_total )
                    {
                        data_seek2 = data_total;
                    }
                    else
                    {
                        break;
                    }
                }

                json_item_len = data_seek2 - data_seek;
                json_item.resize ( json_item_len );
                if ( fread ( & json_item[ 0 ], 1, json_item_len, d_fp ) != json_item_len )
                {
                    break;
                }

                ctxt->value += json_item;
                ctxt->value += ",";

                data_seek   = data_seek2;
                index_seek  += sizeof ( uint64_t );
                
                real_size ++;
            }

            r = 0;
            
        }
        while ( 0 );
        
        if ( i_fp )
        {
            fclose ( i_fp );
            i_fp = NULL;
        }

        if ( d_fp )
        {
            fclose ( d_fp );
            d_fp = NULL;
        }
    }
    else
    {
        char                min21[ 32 ]           = { };
        char                max21[ 32 ]           = { };
        char                json_numeric[ 32 ]    = { };
        c_str_t             base64_src;
        c_str_t             base64_dst;
        std::string         base64_item;
        std::string         json_item;
        std::string         content;
        
        uint32_t            version               = 0;
        uint32_t            ttl                   = 0;
        uint32_t            total                 = offset + size;
        i_iterator_t *      it                    = NULL;

        do
        {

            if ( max > 0 )
            {
                sprintf ( max21, "%021lu", max );
            }

            it = kv->iterator ();
            if ( NULL == it )
            {
                LOG_ERROR ( "[kv_array][export_db_mem]iterator() failed" );
                r = EFAULT;
                break;
            }

            if ( is_seek )
            {
                if ( min > 0 )
                {
                    sprintf ( min21, "%021lu", min );
                    
                    std::string zset_seek ( ctxt->key );
                    zset_seek += min21;

                    it->seek ( zset_seek.c_str (), zset_seek.size () );
                }
                else
                {
                    it->seek ( ctxt->key.c_str (), ctxt->key.size () );
                }
            }
            else
            {
                it->seek_first ();
            }

            for ( i = 0; it->valid () && i < total; it->next (), i ++ )
            {
                if ( i < offset )
                {
                    continue;
                }

                version    = 0;
                ttl        = 0;
                key        = it->key ( & key_len );
                val        = it->value ( & val_len );

                if ( key_len > sizeof ( md5db::block_id_t ) )
                {
                    prefix_tl = key_len - sizeof ( md5db::block_id_t ) - 1;
                    table     = key;

                    if ( key_len > ZSET_SCORE_LEN + sizeof ( md5db::block_id_t ) + 1 &&
                         key [ key_len - ZSET_SCORE_LEN - sizeof ( md5db::block_id_t ) - 1 ] == ZSET_TB
                         )
                    {
                        val_b64   = false;
                        table_len = key_len - ZSET_SCORE_LEN - sizeof ( md5db::block_id_t ) - 1;
                    }
                    else
                    {
                        val_b64   = true;
                        table_len = prefix_tl;
                    }
                    
                    if ( ! is_seek )
                    {
                        if ( key [ table_len ] == ZSET_IN )
                        {
                            continue;
                        }

                        type[ 0 ] = hustdb_t::real_table_type ( key [ table_len ] );
                        type_len  = 1;
                    }
                    else
                    {
                        table_len = 0;
                        type_len  = 0;
                    }
                }
                else
                {
                    prefix_tl = 0;
                    table_len = 0;
                    table     = NULL;
                    type_len  = 0;
                    val_b64   = true;
                }

                if ( unlikely ( is_seek &&
                               ( strncmp ( key, ctxt->key.c_str (), ctxt->key.size () ) != 0 ||
                                 max > 0 &&
                                 ( key_len != ctxt->key.size () + ZSET_SCORE_LEN + sizeof ( md5db::block_id_t ) ||
                                   strncmp ( key + key_len - sizeof ( md5db::block_id_t ) - ZSET_SCORE_LEN, max21, ZSET_SCORE_LEN ) > 0
                                   )
                                 )
                               )
                     )
                {
                    break;
                }

                if ( callback )
                {
                    callback ( callback_param,
                              key,
                              key_len,
                              val,
                              val_len,
                              table,
                              prefix_tl,
                              version,
                              ttl,
                              content,
                              & ignore_this_record,
                              & break_the_loop );
                }

                if ( unlikely ( ignore_this_record || version <= 0 ) )
                {
                    continue;
                }

                export_item_t items[] = {
                    { "key",        key,        key_len,      0,          true     },
                    { "val",        val,        val_len,      0,          val_b64  },
                    { "ver",        NULL,       0,            version,    false    },
                    { "ttl",        NULL,       0,            ttl,        false    },
                    { "tb",         table,      table_len,    0,          false    },
                    { "ty",         type,       type_len,     0,          false    }
                };
                size_t size = sizeof (items ) / sizeof (export_item_t );

                json_item.resize ( 0 );
                json_item += "{";

                for ( size_t j = 0; j < size; ++ j )
                {
                    if ( items [ j ].numeric <= 0 && ( ! items [ j ].data || items [ j ].len <= 0 ) )
                    {
                        continue;
                    }

                    json_item += "\"";
                    json_item += ( char * ) items [ j ].key;
                    json_item += "\":";

                    if ( items [ j ].base64 )
                    {
                        base64_src.len = items [ j ].len;
                        base64_src.data = ( char * ) items [ j ].data;

                        base64_dst.len = c_base64_encoded_length (base64_src.len);
                        base64_item.resize ( base64_dst.len );
                        base64_dst.data = ( char * ) base64_item.c_str ();

                        hustdb_base64_encode ( & base64_src, & base64_dst );

                        json_item += "\"";
                        json_item.append ( base64_dst.data, base64_dst.len );
                        json_item += "\"";
                    }
                    else if ( items [ j ].numeric > 0 )
                    {
                        memset ( json_numeric, 0, sizeof ( json_numeric ) );
                        sprintf ( json_numeric, "%u", items [ j ].numeric );

                        json_item += json_numeric;
                    }
                    else
                    {
                        json_item += "\"";
                        json_item.append ( ( char * ) items [ j ].data, items [ j ].len );
                        json_item += "\"";
                    }

                    json_item += ",";
                }
                
                if ( json_item.size () > 2 )
                {
                    json_item.erase ( json_item.end () - 1, json_item.end () );
                }
                json_item += "}";

                ctxt->value += json_item;
                ctxt->value += ",";

                if ( unlikely ( break_the_loop ) )
                {
                    break;
                }

                real_size ++;
            }

            r = 0;

        }
        while ( 0 );

        if ( it )
        {
            it->kill_me ();
            it = NULL;
        }
    }

    if ( ctxt->value.size () > 2 )
    {
        ctxt->value.erase ( ctxt->value.end () - 1, ctxt->value.end () );
    }
    ctxt->value += "]";

    cb_pm->size = real_size;

    if ( r == 0 )
    {
        rsp = & ctxt->value;
    }

    return r;
}

int kv_array_t::hash_info (
                            int                         user_file_id,
                            int &                       inner_file_id
                            )
{
    int                 r;
    std::vector< int >  other_servers;

    inner_file_id = - 1;

    r = m_hash->hash_with_user_file_id ( user_file_id, other_servers, inner_file_id );
    if ( 0 != r )
    {
        LOG_ERROR ( "[kv_array][hash]hash_with_user_file_id retrn %d, user_file_id=%d", r, user_file_id );
        return r;
    }
    if ( inner_file_id < 0 )
    {
        LOG_ERROR ( "[kv_array][export]user_file_id=%d, not in current server", user_file_id );
        return ENOENT;
    }
    if ( unlikely ( inner_file_id >= ( int ) m_files.size () ) )
    {
        LOG_ERROR ( "[kv_array][export][local=%d][files=%d]invalid local_id",
                   inner_file_id,
                   ( int ) m_files.size () );
        return EINVAL;
    }

    i_kv_t * kv = m_files[ inner_file_id ];
    if ( unlikely ( NULL == kv ) )
    {
        LOG_ERROR ( "[kv_array][export][local=%u]file is NULL", inner_file_id );
        return EFAULT;
    }

    return 0;
}

int kv_array_t::get_user_file_count ( )
{
    return m_hash ? m_hash->get_user_file_count () : 0;
}
