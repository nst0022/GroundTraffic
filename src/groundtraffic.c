/*
 * GroundTraffic
 *
 * (c) Jonathan Harris 2013-2014
 *
 * Licensed under GNU LGPL v2.1.
 */

#include "groundtraffic.h"
#include "planes.h"
#include "bbox.h"

#if IBM
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason, LPVOID lpReserved)
{ return TRUE; }
#endif


/* Globals */
char *pkgpath;
XPLMDataRef ref_plane_lat, ref_plane_lon, ref_view_x, ref_view_y, ref_view_z, ref_rentype, ref_night, ref_monotonic, ref_doy, ref_tod, ref_LOD, ref_cars;
XPLMDataRef ref_datarefs[dataref_count] = { 0 }, ref_varref = 0;
XPLMProbeRef ref_probe;
float lod_bias = DEFAULT_LOD;
airport_t airport = { 0 };
int year=113;		/* Current year (in GMT tz) since 1900 */

/* Published DataRefs. Must be in same order as dataref_t */
const char datarefs[dataref_count][60] = {
    REF_DISTANCE, REF_SPEED, REF_STEER, REF_NODE_LAST, REF_NODE_LAST_DISTANCE, REF_NODE_NEXT, REF_NODE_NEXT_DISTANCE,
#ifdef DEBUG
    REF_LOD, REF_RANGE,
#endif
#ifdef DO_BENCHMARK
    REF_DRAWTIME,
#endif
};

/* In this file */
static XPLMWindowID labelwin = 0;

static float flightcallback(float inElapsedSinceLastCall, float inElapsedTimeSinceLastFlightLoop, int inCounter, void *inRefcon);
static float floatrefcallback(XPLMDataRef inRefCon);
static int intrefcallback(XPLMDataRef inRefCon);
static int varrefcallback(XPLMDataRef inRefCon, float *outValues, int inOffset, int inMax);
static int check_collisions(airport_t *airport);
static int lookup_objects(airport_t *airport);


/* inlines */
static inline int intilerange(dloc_t loc)
{
    double tile_lat, tile_lon;
    tile_lat = floor(XPLMGetDatad(ref_plane_lat));
    tile_lon = floor(XPLMGetDatad(ref_plane_lon));
    return ((abs(tile_lat - floor(loc.lat)) <= TILE_RANGE) && (abs(tile_lon - floor(loc.lon)) <= TILE_RANGE));
}


PLUGIN_API int XPluginStart(char *outName, char *outSignature, char *outDescription)
{
    char buffer[PATH_MAX], *c;
    time_t t;
    struct tm *tm;

    sprintf(outName, "GroundTraffic v%.2f", VERSION);
    strcpy(outSignature, "Marginal.GroundTraffic");
    strcpy(outDescription, "Shows animated airport ground vehicle traffic. Licensed under LGPL v2.1.");

    ref_plane_lat=XPLMFindDataRef("sim/flightmodel/position/latitude");
    ref_plane_lon=XPLMFindDataRef("sim/flightmodel/position/longitude");
    ref_view_x   =XPLMFindDataRef("sim/graphics/view/view_x");
    ref_view_y   =XPLMFindDataRef("sim/graphics/view/view_y");
    ref_view_z   =XPLMFindDataRef("sim/graphics/view/view_z");
    ref_rentype  =XPLMFindDataRef("sim/graphics/view/world_render_type");
    ref_night    =XPLMFindDataRef("sim/graphics/scenery/percent_lights_on");
    ref_monotonic=XPLMFindDataRef("sim/time/total_running_time_sec");
    ref_doy      =XPLMFindDataRef("sim/time/local_date_days");
    ref_tod      =XPLMFindDataRef("sim/time/local_time_sec");
    ref_LOD      =XPLMFindDataRef("sim/private/controls/reno/LOD_bias_rat");
    ref_cars     =XPLMFindDataRef("sim/private/controls/reno/draw_cars_05");
    ref_probe    =XPLMCreateProbe(xplm_ProbeY);
    if (!(ref_view_x && ref_view_y && ref_view_z && ref_night && ref_monotonic && ref_doy && ref_tod && setup_plane_refs())) return xplog("Can't access X-Plane DataRefs!");

    XPLMEnableFeature("XPLM_WANTS_REFLECTIONS", 0);	/* Let's assume there aren't a lot of puddles around */
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);	/* Get paths in posix format */
    XPLMGetPluginInfo(XPLMGetMyID(), NULL, buffer, NULL, NULL);

#if IBM
#  if _WIN64
    strcat(buffer, "\\..\\..\\..\\..");	/* Windows and Linux 64bit plugins are another level down */
#  else
    strcat(buffer, "\\..\\..\\..");
#  endif
    if (!(pkgpath=_fullpath(NULL, buffer, PATH_MAX))) return xplog("Can't find my scenery folder");
    if (pkgpath[strlen(pkgpath)-1]=='\\') pkgpath[strlen(pkgpath)-1]='\0';	/* trim trailing \ */
    for (c=pkgpath+strlen(pkgpath); *(c-1)!='\\' && c>pkgpath; c--);		/* basename */
#else
#  if APL
    strcat(buffer, "/../../..");
    pkgpath=realpath(buffer, NULL);
#  else /* Linux */
    pkgpath=dirname(dirname(dirname(buffer)));
#    if _LP64
    pkgpath=dirname(pkgpath);		/* Windows and Linux 64bit plugins are another level down */
#    endif
    pkgpath=strdup(pkgpath);
#  endif
    if (!pkgpath || !strcmp(pkgpath, ".")) return xplog("Can't find my scenery folder");
    for (c=pkgpath+strlen(pkgpath); *(c-1)!='/' && c>pkgpath; c--);		/* basename */
#endif
    if (!strcasecmp(c, "Resources"))
        return xplog("This plugin should be installed in a scenery package folder!");	/* Fail */
    strcat(outName, " ");
    strcat(outName, c);
    strcat(outSignature, ".");
    strcat(outSignature, c);
    
    srand(time(NULL));	/* Seed rng */
    if (time(&t)!=-1 && (tm = localtime(&t)))	year=tm->tm_year;			/* What year is it? */

    XPLMRegisterFlightLoopCallback(flightcallback, -1 - (rand() % ACTIVE_POLL), NULL);	/* Spread out poll across frames */

    return 1;
}


PLUGIN_API void XPluginStop(void)
{
    XPLMUnregisterFlightLoopCallback(flightcallback, NULL);
    XPLMDestroyProbe(ref_probe);
}

PLUGIN_API int XPluginEnable(void)
{
    readconfig(pkgpath, &airport);
    return 1;
}

PLUGIN_API void XPluginDisable(void)
{
    clearconfig(&airport);
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, long inMessage, void *inParam)
{
    if (inMessage==XPLM_MSG_AIRPORT_LOADED)
    {
        readconfig(pkgpath, &airport);

        /* Check if we've gone out of range so that the old airport is deactivated and per-route DataRefs
           unregistered before the new airport is activated and tries to register those same DataRefs. */
        if (airport.state == active)
        {
            if (!intilerange(airport.tower))
            {
                deactivate(&airport);
            }
            else
            {
                double airport_x, airport_y, airport_z;
                float view_x, view_y, view_z;

                XPLMWorldToLocal(airport.tower.lat, airport.tower.lon, airport.tower.alt, &airport_x, &airport_y, &airport_z);
                view_x=XPLMGetDataf(ref_view_x);
                view_y=XPLMGetDataf(ref_view_y);
                view_z=XPLMGetDataf(ref_view_z);

                if (!indrawrange(((float)airport_x)-view_x, ((float)airport_y)-view_y, ((float)airport_z)-view_z, airport.active_distance+ACTIVE_HYSTERESIS))
                    deactivate(&airport);
            }
        }
    }
    else if (inMessage==XPLM_MSG_SCENERY_LOADED)
    {
        /* Changing the world detail distance setting causes a reload */
        if (ref_LOD) lod_bias = XPLMGetDataf(ref_LOD);

        /* Scenery might be being reloaded with different "runways follow terrain" setting.
         * So (re)run proberoutes(). Do this immediately to prevent indrawrange() failing.  */
        if (airport.state == active)
        {
            if (intilerange(airport.tower))
                proberoutes(&airport);
            else
                deactivate(&airport);
        }
    }
    else if (inMessage==XPLM_MSG_PLANE_LOADED)
    {
        reset_planes();
    }
}


/* Flight loop callback for checking whether we've come into or gone out of range */
static float flightcallback(float inElapsedSinceLastCall, float inElapsedTimeSinceLastFlightLoop, int inCounter, void *inRefcon)
{
    if (airport.state == inactive)
    {
        if (intilerange(airport.tower))
        {
            double airport_x, airport_y, airport_z;
            float view_x, view_y, view_z;

            if (airport.tower.alt == (double) INVALID_ALT)
                proberoutes(&airport);	/* First time we've encountered our airport. Determine elevations. */

            XPLMWorldToLocal(airport.tower.lat, airport.tower.lon, airport.tower.alt, &airport_x, &airport_y, &airport_z);
            view_x=XPLMGetDataf(ref_view_x);
            view_y=XPLMGetDataf(ref_view_y);
            view_z=XPLMGetDataf(ref_view_z);

            if (indrawrange(((float)airport_x)-view_x, ((float)airport_y)-view_y, ((float)airport_z)-view_z, airport.active_distance))
                if (!activate(&airport))	/* Going active */
                    clearconfig(&airport);
        }
    }
    else if (airport.state == active)
    {
        /* Do this check here rather than in drawcallback() because we can't delete labelwin in the middle of the draw callbacks */
        double airport_x, airport_y, airport_z;
        float view_x, view_y, view_z;

        XPLMWorldToLocal(airport.tower.lat, airport.tower.lon, airport.tower.alt, &airport_x, &airport_y, &airport_z);
        view_x=XPLMGetDataf(ref_view_x);
        view_y=XPLMGetDataf(ref_view_y);
        view_z=XPLMGetDataf(ref_view_z);

        if (!indrawrange(((float)airport_x)-view_x, ((float)airport_y)-view_y, ((float)airport_z)-view_z, airport.active_distance+ACTIVE_HYSTERESIS))
            deactivate(&airport);
    }

    return -ACTIVE_POLL;
}


/* Log to Log.txt. Returns 0 because that's a failure return code from most XP entry points */
int xplog(char *msg)
{
    XPLMDebugString("GroundTraffic: ");
    XPLMDebugString(msg);
    XPLMDebugString("\n");
    return 0;
}


/* Identify the route for per-route dataref accessor callbacks */
static inline route_t *datarefroute()
{
    if (drawroute)
    {
        /* We're in the middle of XPLMDrawObjects() in drawroutes() */
        drawroute->state.hasdataref = -1;
        return drawroute;
    }
    else
    {
	/* Probably DataRefEditor calling - return data for first route */
        return (airport.state == active) ? airport.firstroute : NULL;
    }
}


/* dataref accesor callback */
static float floatrefcallback(XPLMDataRef inDataRef)
{
    route_t *route;
    if (!(route = datarefroute())) return 0;

    switch ((dataref_t) ((intptr_t) inDataRef))
    {
    case distance:
        return route->distance;
    case speed:
        if (route->state.frozen||route->state.paused||route->state.waiting||route->state.dataref||route->state.collision)
            return 0;
        else if (route->state.backingup)
            return -route->speed;
        else
            return route->speed;
    case steer:
        return route->steer;
    case node_last_distance:
        return route->distance - route->last_distance;
    case node_next_distance:
        return route->next_distance - (route->distance - route->last_distance);
#ifdef DEBUG
    case lod:
        return route->object.drawlod * lod_factor;
    case range:
    {
        float range_x = route->drawinfo->x - XPLMGetDataf(ref_view_x);
        float range_y = route->drawinfo->y - XPLMGetDataf(ref_view_y);
        float range_z = route->drawinfo->z - XPLMGetDataf(ref_view_z);
        return sqrtf(range_x*range_x + range_y*range_y + range_z*range_z);
    }
#endif
#ifdef DO_BENCHMARK
    case drawtime:
        return drawframes ? (float) drawcumul / (float) drawframes : 0;
#endif
    default:
        return 0;
    }
}


/* dataref accesor callback */
static int intrefcallback(XPLMDataRef inRefcon)
{
    route_t *route;
    if (!(route = datarefroute())) return 0;

    switch ((dataref_t) ((intptr_t) inRefcon))
    {
    case node_last:
        return route->last_node;
    case node_next:
        return route->next_node;
    default:
        return 0;
    }
}


/* dataref accesor callback */
static int varrefcallback(XPLMDataRef inRefCon, float *outValues, int inOffset, int inMax)
{
    int i;
    route_t *route;

    if (outValues==NULL)
        return MAX_VAR;
    else if (!(route = datarefroute()) || inMax<=0 || inOffset<0 || inOffset>=MAX_VAR || !route->varrefs)
        return 0;

    if (inMax+inOffset > MAX_VAR)
        inMax=MAX_VAR-inOffset;

    for (i=0; i<inMax; i++)
        outValues[i]=userrefcallback(*route->varrefs + inOffset + i);

    return inMax;
}


/* Fast cos-like function, but expects input in range -1..1 representing -PI..PI, and scales return from 1..-1 to range 1..0
 * Adapted from http://www.coranac.com/2009/07/sines/ */
static inline float cosz(float z)
{
    if (fabsf(z) <= 0.5f)
    {
        z = z+z;
        return 1 - 0.5f * (z*z * ((2.f-((float) M_PI_4)) - z*z * (1.f-((float) M_PI_4))));
    }
    else
    {
        z = 2 - (z+z);
        return 0.5f * (z*z * ((2.f-((float) M_PI_4)) - z*z * (1.f-(((float) M_PI_4)))));
    }
}

/* user-defined dataref accesor callback */
float userrefcallback(XPLMDataRef inRefcon)
{
    userref_t *userref = inRefcon;
    float now;

    assert (inRefcon);
    if (!userref || !userref->start1 || airport.state!=active) return 0;

    now = XPLMGetDataf(ref_monotonic);
    /* userref->duration may be zero so use equality tests to avoid divide by zero */
    if (now <= userref->start1 || now >= userref->start1 + userref->duration)
    {
        /* First out of range. Try second. */
        if (!userref->start2)
        {
            if (now <= userref->start1)
                return userref->slope==rising ? 0 : 1;
            else
                return userref->slope==rising ? 1 : 0;
        }
        else if (now <= userref->start2)
            return userref->slope==rising ? 1 : 0;
        else if (now >= userref->start2 + userref->duration)
            return userref->slope==rising ? 0 : 1;
        else if (userref->curve == linear)
            return userref->slope==rising ? 1 - (now - userref->start2)/userref->duration : (now - userref->start2)/userref->duration;
        else
            return userref->slope==rising ? cosz((now - userref->start2)/userref->duration) : 1 - cosz((now - userref->start2)/userref->duration);
    }
    else if (userref->curve == linear)
        return userref->slope==rising ? (now - userref->start1)/userref->duration : 1 - (now - userref->start1)/userref->duration;
    else
        return userref->slope==rising ? 1 - cosz((now - userref->start1)/userref->duration) : cosz((now - userref->start1)/userref->duration);
}


/*
 * Load object and emulate X-Plane's LOD calculation for scenery objects
 *
 * X-Plane 10 appears to calculate view distance as follows:
 * - Object contains ATTR_LOD statement:
 *     view_distance = max(ATTR_LOD)    * 0.0007 * (screen_width / "sim/private/controls/reno/LOD_bias_rat")
 * - Otherwise:
 *     view_distance = height           * 0.65   * (screen_width / "sim/private/controls/reno/LOD_bias_rat")
 * - Fallback for flat objects (skipped here):
 *     view_distance = min(width,depth) * 0.093  * (screen_width / "sim/private/controls/reno/LOD_bias_rat")
 */
static XPLMObjectRef loadobject(route_t *route)
{
    FILE *h;
    char line[MAX_NAME+64];
    float height=0, lod=0;
    route_t *other;

#ifdef DO_BENCHMARK
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif
    if (!(route->object.objref = XPLMLoadObject(route->object.physical_name))) return NULL;
#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    snprintf(line, MAX_NAME, "%d us in XPLMLoadObject(\"%s\")", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec), route->object.physical_name);
    xplog(line);
    gettimeofday(&t1, NULL);		/* start */
#endif

    /* If we've already loaded this object then use its LOD */
    for (other = airport.routes; other!=route; other = other->next)
        if (other->object.objref == route->object.objref)
        {
            route->object.drawlod = other->object.drawlod;
            return route->object.objref;
        }

    if (!(h=fopen(route->object.physical_name, "r")))
    {
#ifdef DEBUG
        char msg[MAX_NAME+64];
        sprintf(msg, "Can't parse \"%s\"", route->object.physical_name);
        xplog(msg);
#endif
        route->object.drawlod = DEFAULT_DRAWLOD;
        return route->object.objref;
    }

    while (fgets(line, sizeof(line), h))
    {
        float y;
        if (sscanf(line, " VT %*f %f", &y) == 1)
        {
            if (y > height) height = y;
        }
        else if (sscanf(line, " ATTR_LOD %*f %f", &y) == 1)
        {
            if (y > lod) lod = y;
        }
    }
    fclose(h);

    if (lod)
        route->object.drawlod = 0.0007f * lod;
    else if (height)
        route->object.drawlod = 0.65f * height;
    else
    {
#ifdef DEBUG
        char msg[MAX_NAME+64];
        sprintf(msg, "Can't parse \"%s\"", route->object.physical_name);
        xplog(msg);
#endif
        route->object.drawlod = DEFAULT_DRAWLOD;	/* Perhaps a v7 object? */
    }

#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    snprintf(line, MAX_NAME, "%d us in LOD calculation", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
    xplog(line);
#endif

    return route->object.objref;
}


/* Callback for sorting routes by draw order, so that objects are batched together.
 * Would ideally like to sort by texture since that's the most expensive thing, but we don't know that.
 * So settle for sorting by XPLMObjectRef.
 * Drawing code assumes parents come before children so cater to this - which means if the same object is used as
 * a parent and child it will occur in two separate batches. This isn't disastrous and anyway is unlikely to occur
 * in practice since it's unlikely that the same object would be used as both parent and child. */
static int sortroute(const void *a, const void *b)
{
    const route_t *const *ra = a, *const *rb = b;
    if ((*ra)->parent && !(*rb)->parent) return 1;
    if ((*rb)->parent && !(*ra)->parent) return -1;
    return ((*ra)->object.objref > (*rb)->object.objref) - ((*ra)->object.objref < (*rb)->object.objref);	/* Simple (ra->object.objref - rb->object.objref) risks overflow */
}


/* Going active - load resources. Return non-zero if success */
int activate(airport_t *airport)
{
    route_t *route, **routes;
    userref_t *userref;
    extref_t *extref;
    XPLMPluginID PluginID;
    int count, i;

    assert (airport->state==inactive);

    /* Register per-route DataRefs with X-Plane. Do this before loading objects so DataRef lookups work. */
    for(i=0; i<dataref_count; i++)
        ref_datarefs[i] = XPLMRegisterDataAccessor(datarefs[i], (i==node_last || i==node_next) ? xplmType_Int : xplmType_Float, 0,
                                                   intrefcallback, NULL, floatrefcallback, NULL, NULL, NULL,
                                                   NULL, NULL, NULL, NULL, NULL, NULL, (void*) ((intptr_t) i), NULL);
    ref_varref = XPLMRegisterDataAccessor(REF_VAR, xplmType_FloatArray, 0,
                                          NULL, NULL, NULL, NULL, NULL, NULL,
                                          NULL, NULL, varrefcallback, NULL, NULL, NULL, (void*) ((intptr_t) i), NULL);

    /* Register DataRefs with DRE. */
    if ((PluginID = XPLMFindPluginBySignature("xplanesdk.examples.DataRefEditor")) != XPLM_NO_PLUGIN_ID)
    {
        for(i=0; i<dataref_count; i++)
            XPLMSendMessageToPlugin(PluginID, 0x01000000, (void*) datarefs[i]);
        XPLMSendMessageToPlugin(PluginID, 0x01000000, REF_VAR);

        /* Register user DataRefs with DRE. We don't do this at load to avoid cluttering up the display with inactive DataRefs. */
        for (userref = airport->userrefs; userref; userref = userref->next)
            if (userref->ref)
                XPLMSendMessageToPlugin(PluginID, 0x01000000, (void*) userref->name);
    }

    /* Lookup externally published DataRefs */
    for (extref = airport->extrefs; extref; extref=extref->next)
    {
        for (userref = airport->userrefs; userref; userref = userref->next)
            if (!strcmp(extref->name, userref->name))
            {
                /* Don't bother looking up our own DataRef - just refer directly to it */
                extref->ref = userref;
                extref->type = xplmType_Mine;
                break;
            }
        if (!userref && ((extref->ref = XPLMFindDataRef(extref->name))))
            extref->type = XPLMGetDataRefTypes(extref->ref);
        /* Silently fail on failed lookup - like .obj files do */
    }

    /* Check for collisions and lookup library objects on first activation */
    if (!airport->done_first_activation)
    {
        if (!lookup_objects(airport)) return 0;
        if (!check_collisions(airport)) return 0;
        airport->done_first_activation = -1;
    }

    /* Load objects */
    {
        char msg[MAX_NAME+64];
#ifdef DO_BENCHMARK
        struct timeval t1, t2;
        gettimeofday(&t1, NULL);		/* start */
#endif
        for (route = airport->routes; route; route = route->next)
        {
            if (!loadobject(route))
            {
                sprintf(msg, "Can't load object or train \"%s\"", route->object.name);
                return xplog(msg);
            }
            if (route->highway)		/* If previously deactivated, just let it continue when and where it left off */
                route->next_time=0;	/* apart from highways, which always need resetting to maintain spacing */
        }
#ifdef DO_BENCHMARK
        gettimeofday(&t2, NULL);		/* stop */
        sprintf(msg, "%d us in activate loading resources", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
        xplog(msg);
#endif
    }

    /* Sort routes by XPLMObjectRef and assign XPLMDrawInfo_t entries in sequence so objects can be drawn in batches.
     * Rather than actually shuffling the routes around in memory we just sort an array of pointers and then go back
     * and fix up the linked list and pointers into the XPLMDrawInfo_t array. */
    for (count = 0, route = airport->routes; route; count++, route = route->next);
    if (!airport->drawinfo)
    {
        if (!(airport->drawinfo = calloc(count, sizeof(XPLMDrawInfo_t))))
            return xplog("Out of memory!");
        for (i = 0; i<count; airport->drawinfo[i++].structSize = sizeof(XPLMDrawInfo_t));
    }
    if (!(routes = malloc(count * sizeof(route))))
        return xplog("Out of memory!");
    for (i = 0, route = airport->routes; route; route = route->next)
        routes[i++] = route;
    qsort(routes, count, sizeof(route), sortroute);
    airport->routes = routes[0];
    for (i = 0; i < count; i++)
    {
        routes[i]->drawinfo = airport->drawinfo + i;
        routes[i]->next = i < count-1 ? routes[i+1] : NULL;
    }
    free(routes);

    XPLMEnableFeature("XPLM_WANTS_REFLECTIONS", airport->reflections);
    XPLMRegisterDrawCallback(drawcallback, xplm_Phase_Objects, 0, NULL);	/* After other 3D objects */
    if (airport->drawroutes)
        labelwin = XPLMCreateWindow(0, 1, 1, 0, 1, labelcallback, NULL, NULL, NULL);	/* Under the menubar */

    airport->state=active;
    return 2;
}


/* Callback from XPLMLookupObjects to count library objects */
static void countlibraryobjs(const char *inFilePath, void *inRef)
{}	/* Don't need to do anything */


/* Callback from XPLMLookupObjects to load a library object */
static void chooselibraryobj(const char *inFilePath, void *inRef)
{
    route_t *route=inRef;
    if (!(route->deadlocked--))
        route->object.physical_name = strdup(inFilePath);		/* Load the nth object */
}

/* Callback from XPLMLookupObjects to load a highway library object */
static void enumeratehighwayobj(const char *inFilePath, void *inRef)
{
    highway_t *highway=inRef;
    objdef_t *objdef = highway->expanded + (highway->obj_count ++);
    objdef->physical_name = strdup(inFilePath);
}


/* Lookup object names. Turn highways into multiple routes. */
static int lookup_objects(airport_t *airport)
{
    route_t *route;
    float drawcars = ref_cars ? XPLMGetDatai(ref_cars) : DEFAULT_DRAWCARS;
#ifdef DO_BENCHMARK
    char msg[64];
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif

    assert (!airport->done_first_activation);

    for (route = airport->routes; route; route = route->next)
    {
        if (route->highway && !route->parent)		/* Unexpanded highway */
        {
            highway_t *highway = route->highway;
            float path_dist, path_cumul;
            int i;
            int count = 0;	/* Number of physical objects */

            /* Lookup and enumerate highway object names */
            for (i=0; i<MAX_HIGHWAY; i++)
            {
                int thiscount;
                if (!highway->objects[i].name) break;
                thiscount = XPLMLookupObjects(highway->objects[i].name, airport->tower.lat, airport->tower.lon, countlibraryobjs, NULL);
                count += thiscount ? thiscount : 1;
            }
            if (!(highway->expanded = calloc(count, sizeof(objdef_t))))
                return xplog("Out of memory!");
            count = 0;
            for (i=0; i<MAX_HIGHWAY; i++)
            {
                if (!highway->objects[i].name) break;
                if (!XPLMLookupObjects(highway->objects[i].name, airport->tower.lat, airport->tower.lon, enumeratehighwayobj, highway))
                {
                    /* Try local object */
                    struct stat info;
                    objdef_t *objdef = highway->expanded + (highway->obj_count ++);

                    if ((objdef->physical_name = malloc(strlen(pkgpath) + strlen(objdef->name) + 2)))
                    {
                        strcpy(objdef->physical_name, pkgpath);
                        strcat(objdef->physical_name, "/");
                        strcat(objdef->physical_name, highway->objects[i].name);
                        if (stat(objdef->physical_name, &info))	/* stat it to force error if missing even if it's not used */
                        {
                            char msg[MAX_NAME+64];
                            snprintf(msg, sizeof(msg), "Can't find object \"%s\"", highway->objects[i].name);
                            return xplog(msg);
                        }
                    }
                }
                while (count < highway->obj_count)
                {
                    if (!highway->expanded[count].physical_name)
                        return xplog("Out of memory!");
                    highway->expanded[count].offset  = highway->objects[i].offset;
                    highway->expanded[count].heading = highway->objects[i].heading;
                    count++;
                }
            }

            /* Measure route path length */
            path_dist = 0;
            for (i=1; i<route->pathlen; i++)
            {
                path_t *node = route->path+i, *prev = route->path+i-1;
                path_dist += hypotf(node->p.x - prev->p.x, node->p.z - prev->p.z);
            }

            /* This route becomes the parent and always exists even if DataRef draw_cars_05 == 0 */
            {
                objdef_t *objdef = highway->expanded + (rand() / (RAND_MAX / highway->obj_count + 1));
                if (!(route->object.physical_name = strdup(objdef->physical_name)))
                    return xplog("Out of memory!");
                route->object.offset  = objdef->offset;
                route->object.heading = objdef->heading;
            }

            /* Generate zero or more new child routes */
            if (drawcars > 0)
            {
                float spacing = highway->spacing * (drawcars <= 5 ? 6-drawcars : 1);

                for (path_cumul = spacing; path_cumul <= path_dist - (1-HIGHWAY_VARIANCE) * spacing; path_cumul += spacing)
                {
                    route_t *newroute;
                    objdef_t *objdef = highway->expanded + (rand() / (RAND_MAX / highway->obj_count + 1));

                    if (!(newroute = malloc(sizeof(route_t))))
                        return xplog("Out of memory!");
                    memcpy(newroute, route, sizeof(route_t));
                    route->next = newroute;
                    if (!(newroute->object.physical_name = strdup(objdef->physical_name)))
                        return xplog("Out of memory!");
                    newroute->object.offset  = objdef->offset;
                    newroute->object.heading = objdef->heading;
                    newroute->parent = route;
                    newroute->highway_offset = path_cumul + HIGHWAY_VARIANCE * spacing * ((float) rand() / RAND_MAX - 0.5f);
                }
            }

            for (i=0; i<highway->obj_count; free(highway->expanded[i++].physical_name));
            free (highway->expanded);	/* Don't need this any more */
            highway->expanded = NULL;
        }
        else if (route->object.name)	/* Not an expanded highway */
        {
            /* Try library object */
            int count;

            if ((count = XPLMLookupObjects(route->object.name, airport->tower.lat, airport->tower.lon, countlibraryobjs, NULL)))
            {
                /* Pick one at random (temporarily abuse deadlock variable as a counter) */
                route->deadlocked = rand() % count;	/* rand() doesn't give an even distribution; I don't care */
                XPLMLookupObjects(route->object.name, airport->tower.lat, airport->tower.lon, chooselibraryobj, route);
            }
            else
            {
                /* Try local object */
                struct stat info;
                if ((route->object.physical_name = malloc(strlen(pkgpath) + strlen(route->object.name) + 2)))
                {
                    strcpy(route->object.physical_name, pkgpath);
                    strcat(route->object.physical_name, "/");
                    strcat(route->object.physical_name, route->object.name);
                    if (stat(route->object.physical_name, &info))	/* stat it first to suppress misleading error in Log */
                    {
                        char msg[MAX_NAME+64];
                        snprintf(msg, sizeof(msg), "Can't find object or train \"%s\"", route->object.name);
                        return xplog(msg);
                    }
                }
            }
            if (!route->object.physical_name)
                return xplog("Out of memory!");
        }
    }

#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    sprintf(msg, "%d us in activate lookup", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
    xplog(msg);
#endif
    return 1;
}


/* Check for collisions - O(n * log(n) * m^2) ! */
static int check_collisions(airport_t *airport)
{
    route_t *route, *other;
#ifdef DO_BENCHMARK
    char buffer[64];
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif

    assert (!airport->done_first_activation);

    for (route=airport->routes; route; route=route->next)
        if (!route->parent && !route->highway)		/* Skip child routes and highways */
        {
            int rrev = route->path[route->pathlen-1].flags.reverse;

            for (other=route->next; other; other=other->next)
            {
                int orev = other->path[other->pathlen-1].flags.reverse;
                int r0, r1;

                if (other->parent || other->highway || !bbox_intersect(&route->bbox, &other->bbox))
                    continue;	/* Skip child routes and non-intersecting routes */

                for (r0=0; r0 < route->pathlen; r0++)
                {
                    int o0, o1;
                    loc_t *p0, *p1;

                    if ((r1 = r0+1) == route->pathlen)
                    {
                        if (rrev)
                            break;	/* Reversible routes don't circle back */
                        else
                            r1=0;
                    }
                    p0 = &route->path[r0].waypoint;
                    p1 = &route->path[r1].waypoint;

                    for (o0=0; o0 < other->pathlen; o0++)
                    {
                        loc_t *p2, *p3;

                        if ((o1 = o0+1) == other->pathlen)
                        {
                            if (orev)
                                break;	/* Reversible routes don't circle back */
                            else
                                o1=0;
                        }
                        p2 = &other->path[o0].waypoint;
                        p3 = &other->path[o1].waypoint;

                        if ((p1->lat == p3->lat && p1->lon == p3->lon) || loc_intersect(p0, p1, p2, p3))
                        {
                            /* Co-located path segment end nodes or segments intersect = Collision */
                            collision_t *newc;

                            if (!(newc=malloc(sizeof(collision_t))))
                                return xplog("Out of memory!");
                            newc->route = other;
                            newc->node = o0;
                            newc->next = route->path[r0].collisions;
                            route->path[r0].collisions = newc;

                            if (!(newc=malloc(sizeof(collision_t))))
                                return xplog("Out of memory!");
                            newc->route = route;
                            newc->node = r0;
                            newc->next = other->path[o0].collisions;
                            other->path[o0].collisions = newc;
                        }
                    }
                }
            }
        }

#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    sprintf(buffer, "%d us in activate check collisions", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
    xplog(buffer);
#endif

    return 1;
}


/* No longer active - unload any resources */
void deactivate(airport_t *airport)
{
    route_t *route;
    int i;

    if (airport->state!=active) return;

    for(route=airport->routes; route; route=route->next)
    {
        XPLMUnloadObject(route->object.objref);
        route->object.objref=0;
    }

    /* Unregister per-route DataRefs */
    for(i=0; i<dataref_count; i++)
    {
        XPLMUnregisterDataAccessor(ref_datarefs[i]);
        ref_datarefs[i] = 0;
    }
    XPLMUnregisterDataAccessor(ref_varref);
    ref_varref = 0;

    XPLMUnregisterDrawCallback(drawcallback, xplm_Phase_Objects, 0, NULL);
    if (labelwin)
        XPLMDestroyWindow(labelwin);
    labelwin = 0;

    airport->state=inactive;
    last_frame = 0;
}


/* Probe out route paths */
void proberoutes(airport_t *airport)
{
    route_t *route = airport->routes;
    int i;
    double x, y, z, foo, alt;
    XPLMProbeInfo_t probeinfo;
#ifdef DO_BENCHMARK
    char buffer[MAX_NAME];
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif

    /* First find airport location. Probe twice to correct for slant error, since our
     * airport might be up to two tiles away - http://forums.x-plane.org/index.php?showtopic=38688&page=3&#entry566469 */
    probeinfo.structSize = sizeof(XPLMProbeInfo_t);
    XPLMWorldToLocal(airport->tower.lat, airport->tower.lon, 0, &x, &y, &z);	// 1
    XPLMProbeTerrainXYZ(ref_probe, x, y, z, &probeinfo);			// 2
    XPLMLocalToWorld(probeinfo.locationX, probeinfo.locationY, probeinfo.locationZ, &foo, &foo, &alt);	// 3
    XPLMWorldToLocal(airport->tower.lat, airport->tower.lon, alt, &x, &y, &z);	// 4
    XPLMProbeTerrainXYZ(ref_probe, x, y, z, &probeinfo);			// 5
    XPLMLocalToWorld(probeinfo.locationX, probeinfo.locationY, probeinfo.locationZ, &foo, &foo, &alt);
    airport->tower.alt=alt;
    XPLMWorldToLocal(airport->tower.lat, airport->tower.lon, airport->tower.alt, &airport->p.x, &airport->p.y, &airport->p.z);

    while (route)
    {
        if (!route->parent)	/* Children share parents' route paths, so already probed */
            for (i=0; i<route->pathlen; i++)
            {
                path_t *path=route->path+i;

                if (!i)
                {
                    /* Probe first node using tower location */
                    XPLMWorldToLocal(path->waypoint.lat, path->waypoint.lon, airport->tower.alt + PROBE_ALT_FIRST, &x, &y, &z);
                    XPLMProbeTerrainXYZ(ref_probe, x, y, z, &probeinfo);
                    XPLMLocalToWorld(probeinfo.locationX, probeinfo.locationY, probeinfo.locationZ, &foo, &foo, &alt);
                    /* Probe twice since it might be some distance from the tower */
                    XPLMWorldToLocal(path->waypoint.lat, path->waypoint.lon, alt + PROBE_ALT_NEXT, &x, &y, &z);
                    XPLMProbeTerrainXYZ(ref_probe, x, y, z, &probeinfo);
                    XPLMLocalToWorld(probeinfo.locationX, probeinfo.locationY, probeinfo.locationZ, &foo, &foo, &alt);
                    path->waypoint.alt=alt;
                }
                else
                {
                    /* Assume this node is reasonably close to the last node so re-use its altitude to only probe once */
                    XPLMWorldToLocal(path->waypoint.lat, path->waypoint.lon, route->path[i-1].waypoint.alt + PROBE_ALT_NEXT, &x, &y, &z);
                    XPLMProbeTerrainXYZ(ref_probe, x, y, z, &probeinfo);
                    XPLMLocalToWorld(probeinfo.locationX, probeinfo.locationY, probeinfo.locationZ, &foo, &foo, &alt);
                    path->waypoint.alt=alt;
                }
            }
        route = route->next;
    }

#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    sprintf(buffer, "%d us in proberoutes", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
    xplog(buffer);
#endif

    /* Go ahead and map out routes now so that local co-ordinates are valid for highway expansion */
    maproutes(airport);
}

/* Determine OpenGL co-ordinates of route paths */
void maproutes(airport_t *airport)
{
    route_t *route = airport->routes;
#ifdef DO_BENCHMARK
    char buffer[MAX_NAME];
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif

    while (route)
    {
        route->next_y = INVALID_ALT;	/* Need to (re)calculate altitude */

        if (!route->parent)	/* Children share parents' route paths, so already mapped */
        {
            /* doesn't make sense to do bezier turns at start and end waypoints of a reversible or highway route */
            int i;
            int reversible = (route->highway || route->path[route->pathlen-1].flags.reverse) ? 1 : 0;

            for (i=0; i<route->pathlen; i++)
            {
                double x, y, z;
                path_t *path = route->path + i;

                XPLMWorldToLocal(path->waypoint.lat, path->waypoint.lon, path->waypoint.alt, &x, &y, &z);
                path->p.x=x;  path->p.y=y;  path->p.z=z;
            }

            /* Now do bezier turn points */
            for (i = reversible; i < route->pathlen - reversible; i++)
            {
                path_t *this = route->path + i;
                path_t *next = route->path + (i+1) % route->pathlen;
                path_t *last = route->path + (i-1+route->pathlen) % route->pathlen;	/* mod of negative is undefined */
                float dist, ratio;

                if ((this->flags.backup && this->pausetime) || (last->flags.backup && !last->pausetime))
                    continue;	/* Want to be straight aligned at backing-up waypoint */

                /* back */
                dist = sqrtf((last->p.x - this->p.x) * (last->p.x - this->p.x) +
                             (last->p.z - this->p.z) * (last->p.z - this->p.z));
                if (dist < route->speed * TURN_TIME)
                    ratio = 0.5f;	/* Node is too close - put control point halfway */
                else
                    ratio = route->speed * (TURN_TIME/2) / dist;
                this->p1.x = this->p.x + ratio * (last->p.x - this->p.x);
                this->p1.z = this->p.z + ratio * (last->p.z - this->p.z);

                /* fwd */
                dist = sqrtf((next->p.x - this->p.x) * (next->p.x - this->p.x) +
                             (next->p.z - this->p.z) * (next->p.z - this->p.z));
                if (dist < route->speed * TURN_TIME)
                    ratio = 0.5;	/* Node is too close - put control point halfway */
                else
                    ratio = route->speed * (TURN_TIME/2) / dist;
                this->p3.x = this->p.x + ratio * (next->p.x - this->p.x);
                this->p3.z = this->p.z + ratio * (next->p.z - this->p.z);
            }
        }
        route = route->next;
    }
#ifdef DO_BENCHMARK
    gettimeofday(&t2, NULL);		/* stop */
    sprintf(buffer, "%d us in maproutes", (int) ((t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec));
    xplog(buffer);
#endif
}
