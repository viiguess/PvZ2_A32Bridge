.class public final Lcom/a32/InstallIds;
.super Ljava/lang/Object;
.source "InstallIds.java"


# static fields
.field private static cached:Ljava/lang/String;


# direct methods
.method private constructor <init>()V
    .registers 1

    .line 22
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V

    return-void
.end method

.method private static androidId()Ljava/lang/String;
    .registers 9

    const/4 v0, 0x0

    :try_start_1
    const-string v1, "android.app.ActivityThread"

    .line 41
    invoke-static {v1}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;

    move-result-object v1

    const-string v2, "currentApplication"

    const/4 v3, 0x0

    new-array v4, v3, [Ljava/lang/Class;

    invoke-virtual {v1, v2, v4}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;

    move-result-object v1

    new-array v2, v3, [Ljava/lang/Object;

    invoke-virtual {v1, v0, v2}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;

    move-result-object v1

    .line 42
    invoke-virtual {v1}, Ljava/lang/Object;->getClass()Ljava/lang/Class;

    move-result-object v2

    const-string v4, "getContentResolver"

    new-array v5, v3, [Ljava/lang/Class;

    invoke-virtual {v2, v4, v5}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;

    move-result-object v2

    new-array v4, v3, [Ljava/lang/Object;

    invoke-virtual {v2, v1, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;

    move-result-object v1

    const-string v2, "android.provider.Settings$Secure"

    .line 43
    invoke-static {v2}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;

    move-result-object v2

    const-string v4, "getString"

    const/4 v5, 0x2

    new-array v6, v5, [Ljava/lang/Class;

    const-string v7, "android.content.ContentResolver"

    .line 44
    invoke-static {v7}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;

    move-result-object v7

    aput-object v7, v6, v3

    const-class v7, Ljava/lang/String;

    const/4 v8, 0x1

    aput-object v7, v6, v8

    invoke-virtual {v2, v4, v6}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;

    move-result-object v2

    new-array v4, v5, [Ljava/lang/Object;

    aput-object v1, v4, v3

    const-string v1, "android_id"

    aput-object v1, v4, v8

    .line 45
    invoke-virtual {v2, v0, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;

    move-result-object v1

    check-cast v1, Ljava/lang/String;

    if-eqz v1, :cond_64

    .line 47
    invoke-virtual {v1}, Ljava/lang/String;->length()I

    move-result v2

    if-eqz v2, :cond_64

    const-string v2, "9774d56d682e549c"

    invoke-virtual {v1, v2}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z

    move-result v2
    :try_end_60
    .catchall {:try_start_1 .. :try_end_60} :catchall_64

    if-eqz v2, :cond_63

    goto :goto_64

    :cond_63
    move-object v0, v1

    :catchall_64
    :cond_64
    :goto_64
    return-object v0
.end method

.method public static declared-synchronized id()Ljava/lang/String;
    .registers 9

    const-string v0, "/data/data/"

    const-class v1, Lcom/a32/InstallIds;

    monitor-enter v1

    .line 62
    :try_start_5
    sget-object v2, Lcom/a32/InstallIds;->cached:Ljava/lang/String;
    :try_end_7
    .catchall {:try_start_5 .. :try_end_7} :catchall_bb

    if-eqz v2, :cond_b

    monitor-exit v1

    return-object v2

    .line 64
    :cond_b
    :try_start_b
    invoke-static {}, Lcom/a32/InstallIds;->androidId()Ljava/lang/String;

    move-result-object v2

    if-eqz v2, :cond_34

    .line 66
    new-instance v3, Ljava/lang/StringBuilder;

    invoke-direct {v3}, Ljava/lang/StringBuilder;-><init>()V

    invoke-virtual {v3, v2}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v2

    const-string v3, ":"

    invoke-virtual {v2, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v2

    invoke-static {}, Lcom/a32/InstallIds;->packageName()Ljava/lang/String;

    move-result-object v3

    invoke-virtual {v2, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v2

    invoke-virtual {v2}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object v2

    invoke-static {v2}, Lcom/a32/InstallIds;->md5Hex(Ljava/lang/String;)Ljava/lang/String;

    move-result-object v2

    sput-object v2, Lcom/a32/InstallIds;->cached:Ljava/lang/String;
    :try_end_32
    .catchall {:try_start_b .. :try_end_32} :catchall_34

    .line 67
    monitor-exit v1

    return-object v2

    .line 72
    :catchall_34
    :cond_34
    :try_start_34
    invoke-static {}, Ljava/util/UUID;->randomUUID()Ljava/util/UUID;

    move-result-object v2

    invoke-virtual {v2}, Ljava/util/UUID;->toString()Ljava/lang/String;

    move-result-object v2

    const-string v3, "-"

    const-string v4, ""

    invoke-virtual {v2, v3, v4}, Ljava/lang/String;->replace(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;

    move-result-object v2
    :try_end_44
    .catchall {:try_start_34 .. :try_end_44} :catchall_bb

    .line 74
    :try_start_44
    new-instance v3, Ljava/io/File;

    new-instance v4, Ljava/lang/StringBuilder;

    invoke-direct {v4, v0}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V

    invoke-static {}, Lcom/a32/InstallIds;->packageName()Ljava/lang/String;

    move-result-object v0

    invoke-virtual {v4, v0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v0

    const-string v4, "/files"

    invoke-virtual {v0, v4}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v0

    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object v0

    invoke-direct {v3, v0}, Ljava/io/File;-><init>(Ljava/lang/String;)V

    .line 75
    new-instance v0, Ljava/io/File;

    const-string v4, ".a32_install_id"

    invoke-direct {v0, v3, v4}, Ljava/io/File;-><init>(Ljava/io/File;Ljava/lang/String;)V

    .line 76
    invoke-virtual {v0}, Ljava/io/File;->exists()Z

    move-result v4

    if-eqz v4, :cond_9d

    invoke-virtual {v0}, Ljava/io/File;->length()J

    move-result-wide v4

    const-wide/16 v6, 0x20

    cmp-long v8, v4, v6

    if-nez v8, :cond_9d

    const/16 v4, 0x20

    new-array v5, v4, [B

    .line 78
    new-instance v6, Ljava/io/FileInputStream;

    invoke-direct {v6, v0}, Ljava/io/FileInputStream;-><init>(Ljava/io/File;)V
    :try_end_80
    .catchall {:try_start_44 .. :try_end_80} :catchall_b7

    .line 80
    :try_start_80
    invoke-virtual {v6, v5}, Ljava/io/FileInputStream;->read([B)I

    move-result v7

    if-ne v7, v4, :cond_94

    .line 81
    new-instance v0, Ljava/lang/String;

    const-string v3, "US-ASCII"

    invoke-direct {v0, v5, v3}, Ljava/lang/String;-><init>([BLjava/lang/String;)V

    sput-object v0, Lcom/a32/InstallIds;->cached:Ljava/lang/String;
    :try_end_8f
    .catchall {:try_start_80 .. :try_end_8f} :catchall_98

    .line 85
    :try_start_8f
    invoke-virtual {v6}, Ljava/io/FileInputStream;->close()V
    :try_end_92
    .catchall {:try_start_8f .. :try_end_92} :catchall_b7

    .line 82
    monitor-exit v1

    return-object v0

    .line 85
    :cond_94
    :try_start_94
    invoke-virtual {v6}, Ljava/io/FileInputStream;->close()V

    goto :goto_9d

    :catchall_98
    move-exception v0

    invoke-virtual {v6}, Ljava/io/FileInputStream;->close()V

    .line 86
    throw v0

    .line 88
    :cond_9d
    :goto_9d
    invoke-virtual {v3}, Ljava/io/File;->mkdirs()Z

    .line 89
    new-instance v3, Ljava/io/FileOutputStream;

    invoke-direct {v3, v0}, Ljava/io/FileOutputStream;-><init>(Ljava/io/File;)V
    :try_end_a5
    .catchall {:try_start_94 .. :try_end_a5} :catchall_b7

    :try_start_a5
    const-string v0, "US-ASCII"

    .line 91
    invoke-virtual {v2, v0}, Ljava/lang/String;->getBytes(Ljava/lang/String;)[B

    move-result-object v0

    invoke-virtual {v3, v0}, Ljava/io/FileOutputStream;->write([B)V
    :try_end_ae
    .catchall {:try_start_a5 .. :try_end_ae} :catchall_b2

    .line 93
    :try_start_ae
    invoke-virtual {v3}, Ljava/io/FileOutputStream;->close()V

    goto :goto_b7

    :catchall_b2
    move-exception v0

    invoke-virtual {v3}, Ljava/io/FileOutputStream;->close()V

    .line 94
    throw v0
    :try_end_b7
    .catchall {:try_start_ae .. :try_end_b7} :catchall_b7

    .line 98
    :catchall_b7
    :goto_b7
    :try_start_b7
    sput-object v2, Lcom/a32/InstallIds;->cached:Ljava/lang/String;
    :try_end_b9
    .catchall {:try_start_b7 .. :try_end_b9} :catchall_bb

    .line 99
    monitor-exit v1

    return-object v2

    :catchall_bb
    move-exception v0

    monitor-exit v1

    throw v0
.end method

.method public static imei(Ljava/lang/String;)Ljava/lang/String;
    .registers 8

    if-eqz p0, :cond_9

    .line 117
    invoke-virtual {p0}, Ljava/lang/String;->length()I

    move-result v0

    if-lez v0, :cond_9

    return-object p0

    .line 118
    :cond_9
    invoke-static {}, Lcom/a32/InstallIds;->id()Ljava/lang/String;

    move-result-object p0

    const/16 v0, 0x8

    const/16 v1, 0x10

    .line 119
    invoke-virtual {p0, v0, v1}, Ljava/lang/String;->substring(II)Ljava/lang/String;

    move-result-object v0

    invoke-static {v0, v1}, Ljava/lang/Long;->parseLong(Ljava/lang/String;I)J

    move-result-wide v2

    const/16 v0, 0x18

    .line 120
    invoke-virtual {p0, v1, v0}, Ljava/lang/String;->substring(II)Ljava/lang/String;

    move-result-object v4

    invoke-static {v4, v1}, Ljava/lang/Long;->parseLong(Ljava/lang/String;I)J

    move-result-wide v4

    const/16 v6, 0x20

    .line 121
    invoke-virtual {p0, v0, v6}, Ljava/lang/String;->substring(II)Ljava/lang/String;

    move-result-object p0

    invoke-static {p0, v1}, Ljava/lang/Long;->parseLong(Ljava/lang/String;I)J

    move-result-wide v0

    shl-long/2addr v2, v6

    or-long/2addr v2, v4

    const-wide v4, -0x61c8864680b583ebL

    mul-long v0, v0, v4

    xor-long/2addr v0, v2

    const-wide v2, 0x7fffffffffffffffL

    and-long/2addr v0, v2

    .line 122
    invoke-static {v0, v1}, Ljava/lang/Long;->toString(J)Ljava/lang/String;

    move-result-object p0

    .line 123
    :goto_41
    invoke-virtual {p0}, Ljava/lang/String;->length()I

    move-result v0

    const/16 v1, 0xf

    if-ge v0, v1, :cond_59

    new-instance v0, Ljava/lang/StringBuilder;

    const-string v1, "0"

    invoke-direct {v0, v1}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V

    invoke-virtual {v0, p0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object p0

    invoke-virtual {p0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object p0

    goto :goto_41

    :cond_59
    const/4 v0, 0x0

    .line 124
    invoke-virtual {p0, v0, v1}, Ljava/lang/String;->substring(II)Ljava/lang/String;

    move-result-object p0

    return-object p0
.end method

.method public static mac(Ljava/lang/String;)Ljava/lang/String;
    .registers 6

    .line 108
    invoke-static {p0}, Lcom/a32/InstallIds;->usable(Ljava/lang/String;)Z

    move-result v0

    if-eqz v0, :cond_7

    return-object p0

    .line 109
    :cond_7
    invoke-static {}, Lcom/a32/InstallIds;->id()Ljava/lang/String;

    move-result-object p0

    .line 110
    new-instance v0, Ljava/lang/StringBuilder;

    const-string v1, "02"

    invoke-direct {v0, v1}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V

    const/4 v1, 0x1

    :goto_13
    const/4 v2, 0x6

    if-ge v1, v2, :cond_26

    const/16 v2, 0x3a

    .line 111
    invoke-virtual {v0, v2}, Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;

    move-result-object v2

    mul-int/lit8 v3, v1, 0x2

    add-int/lit8 v4, v3, 0x2

    invoke-virtual {v2, p0, v3, v4}, Ljava/lang/StringBuilder;->append(Ljava/lang/CharSequence;II)Ljava/lang/StringBuilder;

    add-int/lit8 v1, v1, 0x1

    goto :goto_13

    .line 112
    :cond_26
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object p0

    return-object p0
.end method

.method private static md5Hex(Ljava/lang/String;)Ljava/lang/String;
    .registers 7
    .annotation system Ldalvik/annotation/Throws;
        value = {
            Ljava/lang/Exception;
        }
    .end annotation

    const-string v0, "MD5"

    .line 54
    invoke-static {v0}, Ljava/security/MessageDigest;->getInstance(Ljava/lang/String;)Ljava/security/MessageDigest;

    move-result-object v0

    const-string v1, "UTF-8"

    invoke-virtual {p0, v1}, Ljava/lang/String;->getBytes(Ljava/lang/String;)[B

    move-result-object p0

    invoke-virtual {v0, p0}, Ljava/security/MessageDigest;->digest([B)[B

    move-result-object p0

    .line 55
    new-instance v0, Ljava/lang/StringBuilder;

    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V

    .line 56
    array-length v1, p0

    const/4 v2, 0x0

    const/4 v3, 0x0

    :goto_18
    if-ge v3, v1, :cond_33

    aget-byte v4, p0, v3

    const/4 v5, 0x1

    new-array v5, v5, [Ljava/lang/Object;

    and-int/lit16 v4, v4, 0xff

    invoke-static {v4}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;

    move-result-object v4

    aput-object v4, v5, v2

    const-string v4, "%02x"

    invoke-static {v4, v5}, Ljava/lang/String;->format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;

    move-result-object v4

    invoke-virtual {v0, v4}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    add-int/lit8 v3, v3, 0x1

    goto :goto_18

    .line 57
    :cond_33
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object p0

    return-object p0
.end method

.method private static packageName()Ljava/lang/String;
    .registers 7
    .annotation system Ldalvik/annotation/Throws;
        value = {
            Ljava/lang/Exception;
        }
    .end annotation

    .line 25
    new-instance v0, Ljava/io/FileInputStream;

    const-string v1, "/proc/self/cmdline"

    invoke-direct {v0, v1}, Ljava/io/FileInputStream;-><init>(Ljava/lang/String;)V

    .line 27
    :try_start_7
    new-instance v1, Ljava/io/ByteArrayOutputStream;

    invoke-direct {v1}, Ljava/io/ByteArrayOutputStream;-><init>()V

    const/16 v2, 0x100

    new-array v2, v2, [B

    .line 29
    invoke-virtual {v0, v2}, Ljava/io/FileInputStream;->read([B)I

    move-result v3

    const/4 v4, 0x0

    const/4 v5, 0x0

    :goto_16
    if-ge v5, v3, :cond_22

    .line 30
    aget-byte v6, v2, v5

    if-eqz v6, :cond_22

    invoke-virtual {v1, v6}, Ljava/io/ByteArrayOutputStream;->write(I)V

    add-int/lit8 v5, v5, 0x1

    goto :goto_16

    :cond_22
    const-string v2, "UTF-8"

    .line 31
    invoke-virtual {v1, v2}, Ljava/io/ByteArrayOutputStream;->toString(Ljava/lang/String;)Ljava/lang/String;

    move-result-object v1

    const/16 v2, 0x3a

    .line 32
    invoke-virtual {v1, v2}, Ljava/lang/String;->indexOf(I)I

    move-result v2

    if-gez v2, :cond_31

    goto :goto_35

    .line 33
    :cond_31
    invoke-virtual {v1, v4, v2}, Ljava/lang/String;->substring(II)Ljava/lang/String;

    move-result-object v1
    :try_end_35
    .catchall {:try_start_7 .. :try_end_35} :catchall_39

    .line 35
    :goto_35
    invoke-virtual {v0}, Ljava/io/FileInputStream;->close()V

    return-object v1

    :catchall_39
    move-exception v1

    invoke-virtual {v0}, Ljava/io/FileInputStream;->close()V

    .line 36
    throw v1
.end method

.method private static usable(Ljava/lang/String;)Z
    .registers 2

    if-eqz p0, :cond_1a

    .line 103
    invoke-virtual {p0}, Ljava/lang/String;->length()I

    move-result v0

    if-lez v0, :cond_1a

    const-string v0, "02:00:00:00:00:00"

    invoke-virtual {p0, v0}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z

    move-result v0

    if-nez v0, :cond_1a

    const-string v0, "00:00:00:00:00:00"

    invoke-virtual {p0, v0}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z

    move-result p0

    if-nez p0, :cond_1a

    const/4 p0, 0x1

    goto :goto_1b

    :cond_1a
    const/4 p0, 0x0

    :goto_1b
    return p0
.end method
