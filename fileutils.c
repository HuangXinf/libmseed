/****************************************************************************
 *
 * Routines to manage files of miniSEED.
 *
 * Written by Chad Trabant
 *   IRIS Data Management Center
 ***************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "libmseed.h"

static int ms_fread (char *buf, int size, int num, FILE *stream);

/* Skip length in bytes when skipping non-data */
#define SKIPLEN 128

/* Initialize the global file reading parameters */
MS3FileParam gMS3FileParam = {NULL, "", NULL, 0, 0, 0, 0, 0};

/**********************************************************************
 * ms3_readmsr:
 *
 * This routine is a simple wrapper for ms3_readmsr_main() that uses
 * the global file reading parameters.  This routine is not thread
 * safe and cannot be used to read more than one file at a time.
 *
 * See the comments with ms3_readmsr_main() for return values and
 * further description of arguments.
 *********************************************************************/
int
ms3_readmsr (MS3Record **ppmsr, const char *msfile, int64_t *fpos,
             int8_t *last, int8_t skipnotdata, int8_t dataflag, int8_t verbose)
{
  MS3FileParam *msfp = &gMS3FileParam;

  return ms3_readmsr_main (&msfp, ppmsr, msfile, fpos, last,
                           skipnotdata, dataflag, NULL, verbose);
} /* End of ms3_readmsr() */

/**********************************************************************
 * ms3_readmsr_r:
 *
 * This routine is a simple wrapper for ms3_readmsr_main() that uses
 * the re-entrant capabilities.  This routine is thread safe and can
 * be used to read more than one file at a time as long as separate
 * MS3FileParam structures are used for each file.
 *
 * See the comments with ms3_readmsr_main() for return values and
 * further description of arguments.
 *********************************************************************/
int
ms3_readmsr_r (MS3FileParam **ppmsfp, MS3Record **ppmsr, const char *msfile,
               int64_t *fpos, int8_t *last, int8_t skipnotdata,
               int8_t dataflag, int8_t verbose)
{
  return ms3_readmsr_main (ppmsfp, ppmsr, msfile, fpos, last,
                           skipnotdata, dataflag, NULL, verbose);
} /* End of ms_readmsr_r() */

/**********************************************************************
 * ms3_shift_msfp:
 *
 * A helper routine to shift (remove bytes from the beginning of) the
 * file reading buffer for a MSFP.  The buffer length, reading offset
 * and file position indicators are all updated as necessary.
 *
 *********************************************************************/
static void
ms3_shift_msfp (MS3FileParam *msfp, int shift)
{
  if (!msfp)
    return;

  if (shift <= 0 && shift > msfp->readlen)
  {
    ms_log (2, "ms_shift_msfp(): Cannot shift buffer, shift: %d, readlen: %d, readoffset: %d\n",
            shift, msfp->readlen, msfp->readoffset);
    return;
  }

  memmove (msfp->rawrec, msfp->rawrec + shift, msfp->readlen - shift);
  msfp->readlen -= shift;

  if (shift < msfp->readoffset)
  {
    msfp->readoffset -= shift;
  }
  else
  {
    msfp->filepos += (shift - msfp->readoffset);
    msfp->readoffset = 0;
  }

  return;
} /* End of ms3_shift_msfp() */

/* Macro to calculate length of unprocessed buffer */
#define MSFPBUFLEN(MSFP) (MSFP->readlen - MSFP->readoffset)

/* Macro to return current reading position */
#define MSFPREADPTR(MSFP) (MSFP->rawrec + MSFP->readoffset)

/**********************************************************************
 * ms3_readmsr_main:
 *
 * This routine will open and read, with subsequent calls, all
 * miniSEED records in specified file.
 *
 * All static file reading parameters are stored in a MS3FileParam
 * struct and returned (via a pointer to a pointer) for the calling
 * routine to use in subsequent calls.  A MS3FileParam struct will be
 * allocated if necessary.  This routine is thread safe and can be
 * used to read multiple files in parallel as long as the file reading
 * parameters are managed appropriately.
 *
 * If *fpos is not NULL it will be updated to reflect the file
 * position (offset from the beginning in bytes) from where the
 * returned record was read.  As a special case, if *fpos is not NULL
 * and the value it points to is less than 0 this will be interpreted
 * as a (positive) starting offset from which to begin reading data.
 *
 * If *last is not NULL it will be set to 1 when the last record in
 * the file is being returned, otherwise it will be 0.
 *
 * If the skipnotdata flag is true any data chunks read that are not
 * valid data records will be skipped.
 *
 * dataflag will be passed directly to msr_unpack().
 *
 * After reading all the records in a file the controlling program
 * should call this routine one last time with msfile set to NULL.
 * This will close the file and free allocated memory.
 *
 * Returns MS_NOERROR and populates an MS3Record struct at *ppmsr on
 * successful read, returns MS_ENDOFFILE on EOF, otherwise returns a
 * libmseed error code (listed in libmseed.h) and *ppmsr is set to
 * NULL.
 *********************************************************************/
int
ms3_readmsr_main (MS3FileParam **ppmsfp, MS3Record **ppmsr, const char *msfile,
                  int64_t *fpos, int8_t *last, int8_t skipnotdata,
                  int8_t dataflag, MS3Selections *selections, int8_t verbose)
{
  MS3FileParam *msfp;
  int parseval = 0;
  int readsize = 0;
  int readcount = 0;
  int retcode = MS_NOERROR;

  if (!ppmsr)
    return MS_GENERROR;

  if (!ppmsfp)
    return MS_GENERROR;

  msfp = *ppmsfp;

  /* Initialize the file read parameters if needed */
  if (!msfp)
  {
    msfp = (MS3FileParam *)malloc (sizeof (MS3FileParam));

    if (msfp == NULL)
    {
      ms_log (2, "ms_readmsr_main(): Cannot allocate memory for MSFP\n");
      return MS_GENERROR;
    }

    /* Redirect the supplied pointer to the allocated params */
    *ppmsfp = msfp;

    msfp->fp = NULL;
    msfp->filename[0] = '\0';
    msfp->rawrec = NULL;
    msfp->readlen = 0;
    msfp->readoffset = 0;
    msfp->filepos = 0;
    msfp->filesize = 0;
    msfp->recordcount = 0;
  }

  /* When cleanup is requested */
  if (msfile == NULL)
  {
    msr3_free (ppmsr);

    if (msfp->fp != NULL)
      fclose (msfp->fp);

    if (msfp->rawrec != NULL)
      free (msfp->rawrec);

    /* If the file parameters are the global parameters reset them */
    if (*ppmsfp == &gMS3FileParam)
    {
      gMS3FileParam.fp = NULL;
      gMS3FileParam.filename[0] = '\0';
      gMS3FileParam.rawrec = NULL;
      gMS3FileParam.readlen = 0;
      gMS3FileParam.readoffset = 0;
      gMS3FileParam.filepos = 0;
      gMS3FileParam.filesize = 0;
      gMS3FileParam.recordcount = 0;
    }
    /* Otherwise free the MS3FileParam */
    else
    {
      free (*ppmsfp);
      *ppmsfp = NULL;
    }

    return MS_NOERROR;
  }

  /* Allocate reading buffer */
  if (msfp->rawrec == NULL)
  {
    if (!(msfp->rawrec = (char *)malloc (MAXRECLEN)))
    {
      ms_log (2, "ms3_readmsr_main(): Cannot allocate memory for read buffer\n");
      return MS_GENERROR;
    }
  }

  /* Sanity check: track if we are reading the same file */
  if (msfp->fp && strncmp (msfile, msfp->filename, sizeof (msfp->filename)))
  {
    ms_log (2, "ms3_readmsr_main() called with a different file name without being reset\n");

    /* Close previous file and reset needed variables */
    if (msfp->fp != NULL)
      fclose (msfp->fp);

    msfp->fp = NULL;
    msfp->readlen = 0;
    msfp->readoffset = 0;
    msfp->filepos = 0;
    msfp->filesize = 0;
    msfp->recordcount = 0;
  }

  /* Open the file if needed, redirect to stdin if file is "-" */
  if (msfp->fp == NULL)
  {
    /* Store the filename for tracking */
    strncpy (msfp->filename, msfile, sizeof (msfp->filename) - 1);
    msfp->filename[sizeof (msfp->filename) - 1] = '\0';

    if (strcmp (msfile, "-") == 0)
    {
      msfp->fp = stdin;
    }
    else
    {
      if ((msfp->fp = fopen (msfile, "rb")) == NULL)
      {
        ms_log (2, "Cannot open file: %s (%s)\n", msfile, strerror (errno));
        msr3_free (ppmsr);

        return MS_GENERROR;
      }
      else
      {
        /* Determine file size */
        struct stat sbuf;

        if (fstat (fileno (msfp->fp), &sbuf))
        {
          ms_log (2, "Cannot open file: %s (%s)\n", msfile, strerror (errno));
          msr3_free (ppmsr);

          return MS_GENERROR;
        }

        msfp->filesize = (int64_t)sbuf.st_size;
      }
    }
  }

  /* Seek to a specified offset if requested */
  if (fpos != NULL && *fpos < 0)
  {
    /* Only try to seek in real files, not stdin */
    if (msfp->fp != stdin)
    {
      if (lmp_fseek64 (msfp->fp, *fpos * -1, SEEK_SET))
      {
        ms_log (2, "Cannot seek in file: %s (%s)\n", msfile, strerror (errno));

        return MS_GENERROR;
      }

      msfp->filepos = *fpos * -1;
      msfp->readlen = 0;
      msfp->readoffset = 0;
    }
  }

  /* Zero the last record indicator */
  if (last)
    *last = 0;

  /* Read data and search for records */
  for (;;)
  {
    /* Read more data into buffer if not at EOF and buffer has less than MINRECLEN
     * or more data is needed for the current record detected in buffer. */
    if (!feof (msfp->fp) && (MSFPBUFLEN (msfp) < MINRECLEN || parseval > 0))
    {
      /* Reset offsets if no unprocessed data in buffer */
      if (MSFPBUFLEN (msfp) <= 0)
      {
        msfp->readlen = 0;
        msfp->readoffset = 0;
      }
      /* Otherwise shift existing data to beginning of buffer */
      else if (msfp->readoffset > 0)
      {
        ms3_shift_msfp (msfp, msfp->readoffset);
      }

      /* Determine read size */
      readsize = (MAXRECLEN - msfp->readlen);

      /* Read data into record buffer */
      readcount = ms_fread (msfp->rawrec + msfp->readlen, 1, readsize, msfp->fp);

      if (readcount != readsize)
      {
        if (!feof (msfp->fp))
        {
          ms_log (2, "Short read of %d bytes starting from %" PRId64 "\n",
                  readsize, msfp->filepos);
          retcode = MS_GENERROR;
          break;
        }
      }

      /* Update read buffer length */
      msfp->readlen += readcount;

      /* File position corresponding to start of buffer */
      if (msfp->fp != stdin)
        msfp->filepos = lmp_ftell64 (msfp->fp) - msfp->readlen;
    }

    /* Attempt to parse record from buffer */
    if (MSFPBUFLEN (msfp) >= MINRECLEN)
    {
      int parselen = MSFPBUFLEN (msfp);

      parseval = msr3_parse (MSFPREADPTR (msfp), parselen, ppmsr, dataflag, verbose);

      /* Record detected and parsed */
      if (parseval == 0)
      {
        if (verbose > 1)
          ms_log (1, "Read record length of %d bytes\n", (*ppmsr)->reclen);

        /* Test if this is the last record if file size is known (not a pipe) */
        if (last && msfp->filesize)
          if ((msfp->filesize - (msfp->filepos + (*ppmsr)->reclen)) < MINRECLEN)
            *last = 1;

        /* Return file position for this record */
        if (fpos)
          *fpos = msfp->filepos;

        /* Update reading offset, file position and record count */
        msfp->readoffset += (*ppmsr)->reclen;
        msfp->filepos += (*ppmsr)->reclen;
        msfp->recordcount++;

        retcode = MS_NOERROR;
        break;
      }
      else if (parseval < 0)
      {
        /* Skip non-data if requested */
        if (skipnotdata)
        {
          if (verbose > 1)
          {
            ms_log (1, "Skipped %d bytes of non-data record at byte offset %" PRId64 "\n",
                    SKIPLEN, msfp->filepos);
          }

          /* Skip SKIPLEN bytes, update reading offset and file position */
          msfp->readoffset += SKIPLEN;
          msfp->filepos += SKIPLEN;
        }
        /* Parsing errors */
        else
        {
          ms_log (2, "Cannot detect record at byte offset %" PRId64 ": %s\n",
                  msfp->filepos, msfile);

          /* Print common errors and raw details of miniSEED 3 if verbose */
          ms3_parse_raw (MSFPREADPTR (msfp), MSFPBUFLEN (msfp), verbose);

          retcode = parseval;
          break;
        }
      }
      else /* parseval > 0 (found record but need more data) */
      {
        /* Check for parse hints that are larger than MAXRECLEN */
        if ((MSFPBUFLEN (msfp) + parseval) > MAXRECLEN)
        {
          if (skipnotdata)
          {
            /* Skip SKIPLEN bytes, update reading offset and file position */
            msfp->readoffset += SKIPLEN;
            msfp->filepos += SKIPLEN;
          }
          else
          {
            retcode = MS_OUTOFRANGE;
            break;
          }
        }
        /* End of file check */
        else if (feof (msfp->fp))
        {
          if (verbose)
          {
            if (msfp->filesize)
              ms_log (1, "Truncated record at byte offset %" PRId64 ", filesize %" PRId64 ": %s\n",
                      msfp->filepos, msfp->filesize, msfile);
            else
              ms_log (1, "Truncated record at byte offset %" PRId64 "\n",
                      msfp->filepos);
          }

          retcode = MS_ENDOFFILE;
          break;
        }
      }
    } /* End of record detection */

    /* Finished when within MINRECLEN from EOF and buffer less than MINRECLEN */
    if ((msfp->filesize - msfp->filepos) < MINRECLEN && MSFPBUFLEN (msfp) < MINRECLEN)
    {
      if (msfp->recordcount == 0)
      {
        if (verbose > 0)
          ms_log (2, "%s: No data records read, not SEED?\n", msfile);
        retcode = MS_NOTSEED;
      }
      else
      {
        retcode = MS_ENDOFFILE;
      }

      break;
    }
  } /* End of reading, record detection and parsing loop */

  /* Cleanup target MS3Record if returning an error */
  if (retcode != MS_NOERROR)
  {
    msr3_free (ppmsr);
  }

  return retcode;
} /* End of ms3_readmsr_main() */

/*********************************************************************
 * ms3_readtracelist:
 *
 * This is a simple wrapper for ms3_readtracelist_selection() that uses
 * no selections.
 *
 * See the comments with ms3_readtracelist_selection() for return
 * values and further description of arguments.
 *********************************************************************/
int
ms3_readtracelist (MS3TraceList **ppmstl, const char *msfile,
                   double timetol, double sampratetol, int8_t pubversion,
                   int8_t skipnotdata, int8_t dataflag, int8_t verbose)
{
  return ms3_readtracelist_selection (ppmstl, msfile, timetol,
                                      sampratetol, NULL,
                                      pubversion, skipnotdata,
                                      dataflag, verbose);
} /* End of ms3_readtracelist() */

/*********************************************************************
 * ms3_readtracelist_timewin:
 *
 * This is a wrapper for ms3_readtraces_selection() that creates a
 * simple selection for a specified time window.
 *
 * See the comments with ms3_readtraces_selection() for return values
 * and further description of arguments.
 *********************************************************************/
int
ms3_readtracelist_timewin (MS3TraceList **ppmstl, const char *msfile,
                           double timetol, double sampratetol,
                           nstime_t starttime, nstime_t endtime, int8_t pubversion,
                           int8_t skipnotdata, int8_t dataflag, int8_t verbose)
{
  MS3Selections selection;
  MS3SelectTime selecttime;

  selection.tsidpattern[0] = '*';
  selection.tsidpattern[1] = '\0';
  selection.timewindows = &selecttime;
  selection.next = NULL;

  selecttime.starttime = starttime;
  selecttime.endtime = endtime;
  selecttime.next = NULL;

  return ms3_readtracelist_selection (ppmstl, msfile, timetol,
                                      sampratetol, &selection,
                                      pubversion, skipnotdata,
                                      dataflag, verbose);
} /* End of ms3_readtracelist_timewin() */

/*********************************************************************
 * ms3_readtracelist_selection:
 *
 * This routine will open and read all miniSEED records in specified
 * file and populate a trace list.  This routine is thread safe.
 *
 * If a MS3Selections list is supplied it will be used to limit which
 * records are added to the trace list.
 *
 * Returns MS_NOERROR and populates an MS3TraceList struct at *ppmstl
 * on successful read, otherwise returns a libmseed error code (listed
 * in libmseed.h).
 *********************************************************************/
int
ms3_readtracelist_selection (MS3TraceList **ppmstl, const char *msfile,
                             double timetol, double sampratetol,
                             MS3Selections *selections, int8_t pubversion,
                             int8_t skipnotdata, int8_t dataflag, int8_t verbose)
{
  MS3Record *msr = 0;
  MS3FileParam *msfp = 0;
  int retcode;

  if (!ppmstl)
    return MS_GENERROR;

  /* Initialize MS3TraceList if needed */
  if (!*ppmstl)
  {
    *ppmstl = mstl3_init (*ppmstl);

    if (!*ppmstl)
      return MS_GENERROR;
  }

  /* Loop over the input file */
  while ((retcode = ms3_readmsr_main (&msfp, &msr, msfile, NULL, NULL,
                                      skipnotdata, dataflag, NULL, verbose)) == MS_NOERROR)
  {
    /* Test against selections if supplied */
    if (selections)
    {
      nstime_t endtime = msr3_endtime (msr);

      if (ms3_matchselect (selections, msr->tsid, msr->starttime, endtime, NULL) == NULL)
      {
        continue;
      }
    }

    /* Add to trace list */
    mstl3_addmsr (*ppmstl, msr, pubversion, 1, timetol, sampratetol);
  }

  /* Reset return code to MS_NOERROR on successful read by ms_readmsr() */
  if (retcode == MS_ENDOFFILE)
    retcode = MS_NOERROR;

  ms3_readmsr_main (&msfp, &msr, NULL, NULL, NULL, 0, 0, NULL, 0);

  return retcode;
} /* End of ms3_readtracelist_selection() */

/*********************************************************************
 * ms_fread:
 *
 * A wrapper for fread that handles EOF and error conditions.
 *
 * Returns the return value from fread.
 *********************************************************************/
static int
ms_fread (char *buf, int size, int num, FILE *stream)
{
  int read = 0;

  read = (int)fread (buf, size, num, stream);

  if (read <= 0 && size && num)
  {
    if (ferror (stream))
      ms_log (2, "ms_fread(): Cannot read input file\n");

    else if (!feof (stream))
      ms_log (2, "ms_fread(): Unknown return from fread()\n");
  }

  return read;
} /* End of ms_fread() */

/***************************************************************************
 * ms_record_handler_int:
 *
 * Internal record handler.  The handler data should be a pointer to
 * an open file descriptor to which records will be written.
 *
 ***************************************************************************/
static void
ms_record_handler_int (char *record, int reclen, void *ofp)
{
  if (fwrite (record, reclen, 1, (FILE *)ofp) != 1)
  {
    ms_log (2, "Error writing to output file\n");
  }
} /* End of ms_record_handler_int() */

/***************************************************************************
 * msr_writemseed:
 *
 * Pack MS3Record data into miniSEED record(s) by calling msr3_pack() and
 * write to a specified file.
 *
 * Returns the number of records written on success and -1 on error.
 ***************************************************************************/
int
msr3_writemseed (MS3Record *msr, const char *msfile, int8_t overwrite,
                 int maxreclen, int8_t encoding, int8_t verbose)
{
  FILE *ofp;
  char *perms = (overwrite) ? "wb" : "ab";
  int64_t packedrecords = 0;

  if (!msr || !msfile)
    return -1;

  /* Open output file or use stdout */
  if (strcmp (msfile, "-") == 0)
  {
    ofp = stdout;
  }
  else if ((ofp = fopen (msfile, perms)) == NULL)
  {
    ms_log (1, "Cannot open output file %s: %s\n", msfile, strerror (errno));

    return -1;
  }

  /* Pack the MS3Record */
  if (msr->numsamples > 0)
  {
    msr->encoding = encoding;
    msr->reclen = maxreclen;

    packedrecords = msr3_pack (msr, &ms_record_handler_int, ofp, NULL, 1, verbose - 1);

    if (packedrecords < 0)
    {
      ms_log (1, "Cannot write miniSEED for %s\n", msr->tsid);
    }
  }

  /* Close file and return record count */
  fclose (ofp);

  return (packedrecords >= 0) ? packedrecords : -1;
} /* End of msr3_writemseed() */

/***************************************************************************
 * mstl3_writemseed:
 *
 * Pack MS3TraceList data into miniSEED records and write to a
 * specified file.
 *
 * Returns the number of records written on success and -1 on error.
 ***************************************************************************/
int
mstl3_writemseed (MS3TraceList *mstl, const char *msfile, int8_t overwrite,
                  int maxreclen, int8_t encoding, int8_t verbose)
{
  MS3TraceID *tid;
  MS3TraceSeg *seg;
  FILE *ofp;
  char *perms = (overwrite) ? "wb" : "ab";
  int64_t segpackedrecords = 0;
  int64_t packedrecords = 0;

  if (!mstl || !msfile)
    return -1;

  /* Open output file or use stdout */
  if (strcmp (msfile, "-") == 0)
  {
    ofp = stdout;
  }
  else if ((ofp = fopen (msfile, perms)) == NULL)
  {
    ms_log (1, "Cannot open output file %s: %s\n", msfile, strerror (errno));

    return -1;
  }

  /* Pack each MS3TraceSeg in the group */
  tid = mstl->traces;
  while (tid)
  {
    seg = tid->first;
    while (seg)
    {
      if (seg->numsamples <= 0)
      {
        seg = seg->next;
        continue;
      }

      fprintf (stderr, "id.seg packing for %s not implemented\n", tid->tsid);
      // Map to a MS3Record container for the Segment and pack it
      //
      //msr->encoding = encoding;
      //msr->reclen = reclen;
      //msr->byteorder = byteorder;
      //
      //segpackedrecords = msr_pack (msr, &ms_record_handler_int, ofp, NULL, 1, verbose - 1);

      if (segpackedrecords < 0)
      {
        ms_log (1, "Cannot write miniSEED for %s\n", tid->tsid);
      }
      else
      {
        packedrecords += segpackedrecords;
      }

      seg = seg->next;
    }

    tid = tid->next;
  }

  /* Close file and return record count */
  fclose (ofp);

  return packedrecords;
} /* End of mstl3_writemseed() */
