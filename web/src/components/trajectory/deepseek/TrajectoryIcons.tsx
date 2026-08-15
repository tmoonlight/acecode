import type { SVGProps } from 'react'

interface IconProps extends Omit<SVGProps<SVGSVGElement>, 'width' | 'height'> {
  size?: number
}

function IconFrame({ size = 16, children, ...props }: IconProps) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      xmlns="http://www.w3.org/2000/svg"
      aria-hidden="true"
      {...props}
    >
      {children}
    </svg>
  )
}

/** DeepSeek ic_ds_search_outline_16. */
export const IconSearchOutline16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <path d="M11.894845 6.647401C11.894845 3.725463 9.534486 1.356779 6.623219 1.35657C3.711786 1.35657 1.351635 3.725338 1.351635 6.647401C1.351843 9.569296 3.711911 11.938273 6.623219 11.938273C9.534361 11.938064 11.894637 9.569171 11.894845 6.647401ZM13.245462 6.647401C13.245254 10.317935 10.280401 13.293613 6.623219 13.293821C2.965871 13.293821 0.000204 10.31806 0 6.647401C0 2.976574 2.965746 0 6.623219 0C10.280526 0.000205 13.245462 2.9767 13.245462 6.647401Z" fill="currentColor" />
    <path d="M16.000417 15.041079L15.044449 16.000433L11.530434 12.473588L12.486298 11.514234L16.000417 15.041079Z" fill="currentColor" />
  </IconFrame>
)

/** DeepSeek ic_ds_check_outline_16. */
export const IconCheckOutline16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <path d="M15.0498 3.92579L8.49512 12.3818C8.25774 12.6881 8.04517 12.9645 7.84668 13.1689C7.63957 13.3823 7.38732 13.5841 7.04492 13.6719C6.86373 13.7183 6.6757 13.7346 6.48926 13.7197C6.13666 13.6915 5.8528 13.5355 5.6123 13.3604C5.38201 13.1926 5.12573 12.9567 4.83984 12.6953L1.03125 9.21289L1.96875 8.1875L5.77734 11.6699C6.08684 11.9529 6.27773 12.1249 6.43066 12.2363C6.50183 12.2882 6.54699 12.3135 6.57324 12.3252C6.58525 12.3305 6.59269 12.3322 6.5957 12.333C6.63317 12.3367 6.66758 12.3335 6.7002 12.3252C6.74849 12.2956 6.78843 12.2642 6.84961 12.2012C6.98138 12.0654 7.13957 11.8628 7.39648 11.5313L13.9502 3.07422L15.0498 3.92579Z" fill="currentColor" />
  </IconFrame>
)

/** DeepSeek ic_ds_chevron_right_outline_14. */
export const IconChevronRightOutline14 = ({ size = 14, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <path d="M5.5 2.15137L5.92383 2.57617L8.65137 5.30273C8.90706 5.55843 9.13382 5.78438 9.29785 5.98828C9.46883 6.20088 9.61756 6.44405 9.66602 6.75C9.69222 6.91565 9.69222 7.08435 9.66602 7.25C9.61756 7.55595 9.46883 7.79912 9.29785 8.01172C9.13382 8.21561 8.90706 8.44157 8.65137 8.69727L5.92383 11.4238L5.5 11.8486L4.65137 11L5.07617 10.5762L7.80273 7.84863C8.07732 7.57405 8.24849 7.40124 8.3623 7.25977C8.46904 7.12709 8.47813 7.07728 8.48047 7.0625C8.48703 7.02105 8.48703 6.97895 8.48047 6.9375C8.47813 6.92272 8.46904 6.87291 8.3623 6.74023C8.24848 6.59876 8.07732 6.42595 7.80273 6.15137L5.07617 3.42383L4.65137 3L5.5 2.15137Z" fill="currentColor" />
  </IconFrame>
)

export const IconCopyOutline16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <rect x="1.25" y="4.75" width="9.75" height="9.75" rx="2.25" stroke="currentColor" strokeWidth="1.35" />
    <path d="M5 3.2A2.45 2.45 0 0 1 7.45.75h4.3A3.5 3.5 0 0 1 15.25 4.25v4.3A2.45 2.45 0 0 1 12.8 11" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" />
  </IconFrame>
)

export const IconSettingsOutline16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <circle cx="8" cy="8" r="2.25" stroke="currentColor" strokeWidth="1.35" />
    <path d="M8 1.1v1.55M8 13.35v1.55M1.1 8h1.55M13.35 8h1.55M3.12 3.12l1.1 1.1M11.78 11.78l1.1 1.1M12.88 3.12l-1.1 1.1M4.22 11.78l-1.1 1.1" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" />
  </IconFrame>
)

export const IconUserOutline16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <path d="M11.0307 5.46369C11.0305 3.78995 9.6734 2.43357 7.99961 2.43357C6.32601 2.43379 4.96972 3.79009 4.96949 5.46369C4.96949 7.13748 6.32587 8.49455 7.99961 8.49477C9.67354 8.49477 11.0307 7.13762 11.0307 5.46369ZM12.3163 5.46369C12.3163 7.84777 10.3837 9.78042 7.99961 9.78042C5.61572 9.7802 3.68288 7.84763 3.68288 5.46369C3.6831 3.07993 5.61586 1.14718 7.99961 1.14695C10.3836 1.14695 12.3161 3.0798 12.3163 5.46369Z" fill="currentColor" />
    <path d="M8.00002 10.3316C11.7343 10.3316 14.1864 11.8997 15.0387 14.4445L13.8197 14.8531C13.1955 12.9893 11.3673 11.6182 8.00002 11.6182C4.63277 11.6182 2.80455 12.9893 2.18031 14.8531L.961304 14.4445C1.81368 11.8997 4.26579 10.3316 8.00002 10.3316Z" fill="currentColor" />
  </IconFrame>
)

export const IconSparkle16 = ({ size = 16, ...props }: IconProps) => (
  <IconFrame size={size} {...props}>
    <path d="M6.1 3.1Q6.6 7.8 11.3 8.3Q6.6 8.8 6.1 13.5Q5.6 8.8.9 8.3Q5.6 7.8 6.1 3.1Z" fill="currentColor" />
    <path d="M11.9 1Q12.2 3.7 14.9 4Q12.2 4.3 11.9 7Q11.6 4.3 8.9 4Q11.6 3.7 11.9 1Z" fill="currentColor" />
    <path d="M12.5 9.4Q12.7 11.4 14.7 11.6Q12.7 11.8 12.5 13.8Q12.3 11.8 10.3 11.6Q12.3 11.4 12.5 9.4Z" fill="currentColor" />
  </IconFrame>
)
